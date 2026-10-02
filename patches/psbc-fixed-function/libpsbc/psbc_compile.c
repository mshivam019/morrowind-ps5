/*
 * libpsbc — SPIR-V to PS4/PS5 Shader Binary Compiler Library
 *
 * Extracted from cmd/psbc/main.c. Contains the full compilation pipeline:
 *   SPIR-V → NIR (via radv_shader_spirv_to_nir)
 *   NIR optimization (radv_optimize_nir, radv_nir_lower_io)
 *   NIR postprocessing (radv_postprocess_nir)
 *   NIR → GCN ISA (via ACO: radv_shader_nir_to_asm)
 *   GCN ISA → GnmShaderFileHeader (buildshaderbinary)
 */

#include "psbc_compile.h"

#include <errno.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#include <pssl_types.h>

#include "aco_interface.h"
#include "ac_gpu_info.h"
#include "ac_shader_util.h"
#include "ac_binary.h"
#include "ac_nir.h"
#include "amd_family.h"
#include "nir/nir_builder.h"
#include "nir/nir_tcs_info.h"
#include "nir/nir_xfb_info.h"
#include "nir/radv_nir.h"
#include "radv_shader.h"
#include "radv_shader_args.h"
#include "radv_shader_info.h"
#include "radv_descriptor_set.h"
#include "radv_aco_shader_info.h"
#include "radv_pipeline.h"
#include "sid.h"

#include "crc32_sb.h"

/* Keep standalone PSBC ABI-compatible with the pinned Mesa NIR build. */
_Static_assert(sizeof(nir_instr_type) == 1, "NIR enums must be packed");
_Static_assert(sizeof(nir_intrinsic_op) == 4, "unexpected NIR intrinsic enum size");
_Static_assert(offsetof(nir_intrinsic_instr, intrinsic) == 56,
               "PSBC/Mesa NIR layout mismatch");

/* ACO packs color exports into consecutive MRT slots. Match RADV's final
 * register emission, but keep CB_SHADER_MASK in logical attachment order. */
static uint32_t compact_spi_shader_col_format(uint32_t formats) {
    uint32_t compacted = 0;
    unsigned slot = 0;
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t format = (formats >> (4 * i)) & 0xf;
        if (format)
            compacted |= format << (4 * slot++);
    }
    return compacted;
}

static void debug_shader_io(const char* label, const nir_shader* nir,
                            const struct radv_shader_info* info) {
    if (!getenv("PSBC_DEBUG_IO"))
        return;
    fprintf(stderr,
            "PSBC IO %s: inputs=0x%016" PRIx64
            " outputs=0x%016" PRIx64
            " ps_inputs=%u ps_mask=0x%08x params=%u prim_params=%u"
            " linked=%u/%u merged_separate=%u slots=%u/%u\n",
            label, nir->info.inputs_read, nir->info.outputs_written,
            info ? info->ps.num_inputs : 0,
            info ? info->ps.input_mask : 0,
            info ? info->outinfo.param_exports : 0,
            info ? info->outinfo.prim_param_exports : 0,
            info ? info->inputs_linked : 0,
            info ? info->outputs_linked : 0,
            info ? info->merged_shader_compiled_separately : 0,
            info && info->stage == MESA_SHADER_VERTEX
                ? info->vs.num_linked_outputs : 0,
            info && info->stage == MESA_SHADER_GEOMETRY
                ? info->gs.num_linked_inputs : 0);
}

/* Run after ABI lowering so both shader ID reads and vertex fetch indices
 * receive the same logical instance, before any attribute divisor. */
static bool lower_instance_id_bias(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
    const struct radv_shader_args *args = data;
    if (!args->ac.instance_id.used ||
        intr->intrinsic != nir_intrinsic_load_vector_arg_amd ||
        nir_intrinsic_base(intr) != args->ac.instance_id.arg_index)
        return false;
    b->cursor = nir_after_instr(&intr->instr);
    nir_def *bias = ac_nir_load_arg(b, &args->ac, args->instance_id_bias);
    nir_def *logical = nir_iadd(b, &intr->def, bias);
    nir_def_rewrite_uses_after(&intr->def, logical);
    return true;
}

static void debug_stage(const char* label) {
    if (!getenv("PSBC_DEBUG_STAGE"))
        return;
    fprintf(stderr, "PSBC stage %s\n", label);
    fflush(stderr);
}

struct gallium_buffer_state {
    const PsbcCompileOptions* options;
    bool valid;
};

static unsigned
contiguous_sampler_count(const PsbcCompileOptions* opts,
                         const PsbcDescriptorBinding* first)
{
    unsigned count = first->array_size;

    while (first->array_size == 1 &&
           first->binding + count < PSBC_MAX_DESCRIPTOR_BINDINGS) {
        const PsbcDescriptorBinding* next = NULL;
        for (unsigned i = 0; i < opts->descriptor_binding_count; ++i) {
            const PsbcDescriptorBinding* candidate = &opts->descriptor_bindings[i];
            if (candidate->set == first->set &&
                candidate->binding == first->binding + count) {
                next = candidate;
                break;
            }
        }
        if (!next || next->type != PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ||
            next->array_size != 1 || next->stride != first->stride ||
            next->offset != first->offset + count * first->stride)
            break;
        ++count;
    }
    return count;
}

/* Gallium scalar slots must become resource tuples before RADV descriptor
 * lowering. Keep existing tuples/descriptors intact. Reject invalid layouts
 * here instead of letting a later NIR pass abort the application. */
static bool lower_gallium_buffer_index(nir_builder* b, nir_instr* instruction,
                                      void* data) {
    struct gallium_buffer_state* state = data;
    const PsbcCompileOptions* opts = state->options;
    if (instruction->type != nir_instr_type_intrinsic)
        return false;
    nir_intrinsic_instr* intrinsic = nir_instr_as_intrinsic(instruction);
    unsigned source = 0;
    const bool ubo = intrinsic->intrinsic == nir_intrinsic_load_ubo;
    switch (intrinsic->intrinsic) {
    case nir_intrinsic_load_ubo:
    case nir_intrinsic_load_ssbo:
    case nir_intrinsic_get_ssbo_size:
    case nir_intrinsic_ssbo_atomic:
    case nir_intrinsic_ssbo_atomic_swap:
        break;
    case nir_intrinsic_store_ssbo:
        source = 1;
        break;
    default:
        return false;
    }
    nir_src slot = intrinsic->src[source];
    if (slot.ssa->num_components != 1)
        return false;
    if (slot.ssa->bit_size != 32 ||
        (!opts->gallium_buffer_arrays &&
         (!ubo || !nir_src_is_const(slot) ||
          nir_src_as_uint(slot) >= PSBC_MAX_DESCRIPTOR_BINDINGS -
                                      PSBC_GALLIUM_UBO_BINDING_BASE))) {
        state->valid = false;
        return false;
    }
    /* Mesa's six GL stages are VS/TCS/TES/GS/FS/CS in that order. */
    unsigned binding = opts->gallium_buffer_arrays
        ? 4u * b->shader->info.stage + (ubo ? 1u : 2u)
        : PSBC_GALLIUM_UBO_BINDING_BASE + nir_src_as_uint(slot);
    const PsbcDescriptorType type = ubo ? PSBC_DESCRIPTOR_UNIFORM_BUFFER
                                       : PSBC_DESCRIPTOR_STORAGE_BUFFER;
    const PsbcDescriptorBinding* descriptor = NULL;
    for (unsigned i = 0; i < opts->descriptor_binding_count; ++i)
        if (opts->descriptor_bindings[i].binding == binding &&
            opts->descriptor_bindings[i].type == type)
            descriptor = &opts->descriptor_bindings[i];
    if (!descriptor || (opts->gallium_buffer_arrays && nir_src_is_const(slot) &&
                        nir_src_as_uint(slot) >= descriptor->array_size)) {
        state->valid = false;
        return false;
    }

    b->cursor = nir_before_instr(instruction);
    nir_def* resource = nir_vulkan_resource_index(
        b, 3, 32, opts->gallium_buffer_arrays ? slot.ssa : nir_imm_int(b, 0),
        .desc_set = 0, .binding = binding,
        .desc_type = ubo ? nir_descriptor_type_uniform_buffer
                         : nir_descriptor_type_storage_buffer,
        .resource_type = ubo ? nir_resource_type_uniform_buffer
                             : nir_resource_type_read_write_storage_buffer);
    nir_src_rewrite(&intrinsic->src[source], resource);
    return true;
}

/* 32-bit R/RG/RGBA, 16-bit RGBA and RGBA8_UNORM images. Atomics remain
 * restricted to scalar integer formats with matching native descriptors. */
static bool lower_gallium_image_index(nir_builder* b, nir_instr* instruction,
                                     void* data) {
    struct gallium_buffer_state* state = data;
    if (instruction->type != nir_instr_type_intrinsic)
        return false;
    nir_intrinsic_instr* intr = nir_instr_as_intrinsic(instruction);
    switch (intr->intrinsic) {
    case nir_intrinsic_image_load:
    case nir_intrinsic_image_store:
    case nir_intrinsic_image_atomic:
    case nir_intrinsic_image_atomic_swap:
    case nir_intrinsic_image_size:
    case nir_intrinsic_image_samples:
        break;
    default:
        return false;
    }
    const PsbcCompileOptions* opts = state->options;
    const unsigned key = 4u * b->shader->info.stage + 3u;
    const PsbcDescriptorBinding* bank = NULL;
    for (unsigned i = 0; i < opts->descriptor_binding_count; ++i)
        if (opts->descriptor_bindings[i].binding == key &&
            opts->descriptor_bindings[i].type == PSBC_DESCRIPTOR_STORAGE_IMAGE)
            bank = &opts->descriptor_bindings[i];
    nir_src slot = intr->src[0];
    const enum pipe_format format = nir_intrinsic_format(intr);
    const bool atomic = intr->intrinsic == nir_intrinsic_image_atomic ||
                        intr->intrinsic == nir_intrinsic_image_atomic_swap;
    const bool query = intr->intrinsic == nir_intrinsic_image_samples;
    /* A formatless writeonly image uses the bound descriptor's format.
     * Preserve its GLSL numeric type; loads and atomics still require formats. */
    const nir_alu_type store_type = format == PIPE_FORMAT_NONE &&
        intr->intrinsic == nir_intrinsic_image_store ? nir_intrinsic_src_type(intr) : 0;
    const bool uint_format =
        store_type == nir_type_uint32 ||
        format == PIPE_FORMAT_R8_UINT || format == PIPE_FORMAT_R8G8_UINT ||
        format == PIPE_FORMAT_R8G8B8A8_UINT || format == PIPE_FORMAT_R16_UINT ||
        format == PIPE_FORMAT_R16G16_UINT || format == PIPE_FORMAT_R16G16B16A16_UINT ||
        format == PIPE_FORMAT_R32_UINT || format == PIPE_FORMAT_R32G32_UINT ||
        format == PIPE_FORMAT_R32G32B32A32_UINT || format == PIPE_FORMAT_R10G10B10A2_UINT;
    const bool sint_format =
        store_type == nir_type_int32 ||
        format == PIPE_FORMAT_R8_SINT || format == PIPE_FORMAT_R8G8_SINT ||
        format == PIPE_FORMAT_R8G8B8A8_SINT || format == PIPE_FORMAT_R16_SINT ||
        format == PIPE_FORMAT_R16G16_SINT || format == PIPE_FORMAT_R16G16B16A16_SINT ||
        format == PIPE_FORMAT_R32_SINT || format == PIPE_FORMAT_R32G32_SINT ||
        format == PIPE_FORMAT_R32G32B32A32_SINT;
    const bool float_format =
        query ||
        store_type == nir_type_float32 ||
        format == PIPE_FORMAT_R8_UNORM || format == PIPE_FORMAT_R8_SNORM ||
        format == PIPE_FORMAT_R8G8_UNORM || format == PIPE_FORMAT_R8G8_SNORM ||
        format == PIPE_FORMAT_R8G8B8A8_UNORM || format == PIPE_FORMAT_R8G8B8A8_SNORM ||
        format == PIPE_FORMAT_R16_UNORM || format == PIPE_FORMAT_R16_SNORM ||
        format == PIPE_FORMAT_R16G16_UNORM || format == PIPE_FORMAT_R16G16_SNORM ||
        format == PIPE_FORMAT_R16G16B16A16_UNORM || format == PIPE_FORMAT_R16G16B16A16_SNORM ||
        format == PIPE_FORMAT_R16_FLOAT || format == PIPE_FORMAT_R16G16_FLOAT ||
        format == PIPE_FORMAT_R16G16B16A16_FLOAT || format == PIPE_FORMAT_R32_FLOAT ||
        format == PIPE_FORMAT_R32G32_FLOAT || format == PIPE_FORMAT_R32G32B32A32_FLOAT ||
        format == PIPE_FORMAT_R10G10B10A2_UNORM || format == PIPE_FORMAT_R11G11B10_FLOAT;
    const bool scalar_integer = format == PIPE_FORMAT_R32_UINT || format == PIPE_FORMAT_R32_SINT;
    if (!opts->gallium_buffer_arrays || !bank ||
        slot.ssa->num_components != 1 || slot.ssa->bit_size != 32 ||
        (nir_src_is_const(slot) && nir_src_as_uint(slot) >= bank->array_size) ||
        (nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_2D &&
         nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_MS &&
         nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_1D &&
         nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_3D &&
         nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_CUBE &&
         nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_RECT &&
         nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_BUF) ||
        !(uint_format || sint_format || float_format) || (atomic && !scalar_integer)) {
        state->valid = false;
        return false;
    }
    /* Cube-size lowering retains a vec2 destination for a non-array cube,
     * but its normalized 2D-array descriptor produces vec3. Preserve vec2 uses. */
    if (intr->intrinsic == nir_intrinsic_image_size &&
        nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_2D &&
        nir_intrinsic_image_array(intr) && intr->def.num_components == 2) {
        intr->num_components = intr->def.num_components = 3;
        b->cursor = nir_after_instr(instruction);
        nir_def* size = nir_trim_vector(b, &intr->def, 2);
        nir_def_rewrite_uses_after(&intr->def, size);
    }
    b->cursor = nir_before_instr(instruction);
    const enum glsl_base_type type = uint_format ? GLSL_TYPE_UINT :
                                    sint_format ? GLSL_TYPE_INT : GLSL_TYPE_FLOAT;
    const glsl_type* image = glsl_image_type(nir_intrinsic_image_dim(intr),
        nir_intrinsic_image_array(intr), type);
    nir_variable* var = nir_variable_create(b->shader, nir_var_image,
        glsl_array_type(image, bank->array_size, 0), "gallium_storage_images");
    var->data.descriptor_set = 0;
    var->data.binding = key;
    var->data.image.format = format;
    nir_deref_instr* deref = nir_build_deref_array(b, nir_build_deref_var(b, var), slot.ssa);
    nir_rewrite_image_intrinsic(intr, &deref->def, nir_image_intrinsic_type_deref);
    return true;
}

static bool lower_gallium_texture_index(nir_builder* b, nir_instr* instruction,
                                       void* data) {
    struct gallium_buffer_state* state = data;
    if (instruction->type != nir_instr_type_tex)
        return false;
    nir_tex_instr* tex = nir_instr_as_tex(instruction);
    const int texture_offset = nir_tex_instr_src_index(tex, nir_tex_src_texture_offset);
    const int sampler_offset = nir_tex_instr_src_index(tex, nir_tex_src_sampler_offset);
    if (texture_offset < 0 && sampler_offset < 0)
        return false;
    if (texture_offset < 0 || (nir_tex_instr_need_sampler(tex) && sampler_offset >= 0 &&
        tex->src[sampler_offset].src.ssa != tex->src[texture_offset].src.ssa)) {
        state->valid = false;
        return false;
    }
    const PsbcDescriptorBinding* bank = NULL;
    for (unsigned i = 0; i < state->options->descriptor_binding_count; ++i) {
        const PsbcDescriptorBinding* candidate = &state->options->descriptor_bindings[i];
        if (!candidate->set && candidate->binding == tex->texture_index &&
            candidate->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER)
            bank = candidate;
    }
    enum glsl_base_type base_type;
    switch (nir_alu_type_get_base_type(tex->dest_type)) {
    case nir_type_float: base_type = GLSL_TYPE_FLOAT; break;
    case nir_type_int: base_type = GLSL_TYPE_INT; break;
    case nir_type_uint: base_type = GLSL_TYPE_UINT; break;
    default:
        state->valid = false;
        return false;
    }
    const unsigned array_size = bank ? contiguous_sampler_count(state->options, bank) : 0;
    if (array_size < 2) {
        state->valid = false;
        return false;
    }
    b->cursor = nir_before_instr(instruction);
    const glsl_type* sampler = glsl_sampler_type(tex->sampler_dim,
        tex->is_shadow, tex->is_array, base_type);
    nir_variable* var = nir_variable_create(b->shader, nir_var_uniform,
        glsl_array_type(sampler, array_size, 0), "gallium_sampler_array");
    var->data.descriptor_set = 0;
    var->data.binding = tex->texture_index;
    nir_deref_instr* deref = nir_build_deref_array(b, nir_build_deref_var(b, var),
        tex->src[texture_offset].src.ssa);
    for (int i = (int)tex->num_srcs - 1; i >= 0; --i)
        if (tex->src[i].src_type == nir_tex_src_texture_offset ||
            tex->src[i].src_type == nir_tex_src_sampler_offset)
            nir_tex_instr_remove_src(tex, i);
    nir_tex_instr_add_src(tex, nir_tex_src_texture_deref, &deref->def);
    if (nir_tex_instr_need_sampler(tex))
        nir_tex_instr_add_src(tex, nir_tex_src_sampler_deref, &deref->def);
    return true;
}

/* === Mesa stage mapping === */

static void psbc_setup_descriptor_sizes(struct radv_compiler_info *info) {
    info->sampled_image_desc_size = 32;
    info->combined_image_sampler_desc_size = 48;
    info->combined_image_sampler_offset = 32;
    info->sampler_descriptor_size = 16;
    info->sampler_descriptor_alignment = 16;
    info->image_descriptor_size = 32;
    info->image_descriptor_alignment = 16;
    info->buffer_descriptor_size = 16;
    info->buffer_descriptor_alignment = 16;
}

static mesa_shader_stage psbc_to_mesa_stage(PsbcStage s) {
    switch (s) {
    case PSBC_STAGE_VERTEX:     return MESA_SHADER_VERTEX;
    case PSBC_STAGE_TESS_CTRL:  return MESA_SHADER_TESS_CTRL;
    case PSBC_STAGE_TESS_EVAL:  return MESA_SHADER_TESS_EVAL;
    case PSBC_STAGE_GEOMETRY:   return MESA_SHADER_GEOMETRY;
    case PSBC_STAGE_FRAGMENT:   return MESA_SHADER_FRAGMENT;
    case PSBC_STAGE_COMPUTE:    return MESA_SHADER_COMPUTE;
    case PSBC_STAGE_TASK:       return MESA_SHADER_TASK;
    case PSBC_STAGE_EXPORT:     return MESA_SHADER_VERTEX;  /* ES is a VS variant */
    case PSBC_STAGE_LOCAL:      return MESA_SHADER_VERTEX;  /* LS is a VS variant */
    default:                    return MESA_SHADER_NONE;
    }
}

/* === Init/shutdown (refcounted) === */

static int g_init_refcount = 0;
static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;

void psbc_init(void) {
    pthread_mutex_lock(&g_init_mutex);
    if (g_init_refcount++ == 0) {
        glsl_type_singleton_init_or_ref();
    }
    pthread_mutex_unlock(&g_init_mutex);
}

void psbc_shutdown(void) {
    pthread_mutex_lock(&g_init_mutex);
    if (g_init_refcount > 0 && --g_init_refcount == 0) {
        glsl_type_singleton_decref();
    }
    pthread_mutex_unlock(&g_init_mutex);
}

/* === PSSL/GNM type mapping === */

static PsslShaderType psbshtype(PsbcStage s) {
    switch (s) {
    case PSBC_STAGE_VERTEX:      return PSSL_SHADER_VS;
    case PSBC_STAGE_TESS_EVAL:   return PSSL_SHADER_VS;  /* DS outputs vertices */
    case PSBC_STAGE_EXPORT:      return PSSL_SHADER_VS;  /* ES is a VS variant */
    case PSBC_STAGE_LOCAL:       return PSSL_SHADER_VS;  /* LS is a VS variant */
    case PSBC_STAGE_FRAGMENT:    return PSSL_SHADER_FS;
    case PSBC_STAGE_COMPUTE:     return PSSL_SHADER_CS;
    case PSBC_STAGE_GEOMETRY:    return PSSL_SHADER_VS;  /* no PSSL GS type; use VS */
    case PSBC_STAGE_TESS_CTRL:   return PSSL_SHADER_VS;  /* no PSSL HS type; use VS */
    default:                      return PSSL_SHADER_VS;
    }
}

static GnmShaderType gnmshtype(PsbcStage s) {
    switch (s) {
    case PSBC_STAGE_VERTEX:      return GNM_SHADER_VERTEX;
    case PSBC_STAGE_TESS_EVAL:   return GNM_SHADER_VERTEX;  /* DS outputs vertices */
    case PSBC_STAGE_EXPORT:      return GNM_SHADER_VERTEX;  /* ES is a VS variant */
    case PSBC_STAGE_LOCAL:       return GNM_SHADER_VERTEX;  /* LS is a VS variant */
    case PSBC_STAGE_FRAGMENT:    return GNM_SHADER_PIXEL;
    case PSBC_STAGE_COMPUTE:     return GNM_SHADER_COMPUTE;
    case PSBC_STAGE_GEOMETRY:    return GNM_SHADER_GEOMETRY;
    case PSBC_STAGE_TESS_CTRL:   return GNM_SHADER_HULL;
    default:                      return GNM_SHADER_VERTEX;
    }
}

static GnmShaderBinaryType shbintype(PsbcStage s) {
    switch (s) {
    case PSBC_STAGE_VERTEX:      return GNM_SHB_VS_VS;
    case PSBC_STAGE_TESS_EVAL:   return GNM_SHB_DS_VS;
    case PSBC_STAGE_EXPORT:      return GNM_SHB_VS_ES;
    case PSBC_STAGE_LOCAL:       return GNM_SHB_VS_LS;
    case PSBC_STAGE_FRAGMENT:    return GNM_SHB_PS;
    case PSBC_STAGE_COMPUTE:     return GNM_SHB_CS;
    case PSBC_STAGE_GEOMETRY:    return GNM_SHB_GS;
    case PSBC_STAGE_TESS_CTRL:   return GNM_SHB_HS;
    default:                      return GNM_SHB_VS_VS;
    }
}

static uint32_t headershsize(PsbcStage s) {
    switch (s) {
    case PSBC_STAGE_VERTEX:      return sizeof(GnmVsShader);
    case PSBC_STAGE_TESS_EVAL:   return sizeof(GnmVsShader);  /* DS uses VS struct */
    case PSBC_STAGE_EXPORT:      return sizeof(GnmEsShader);
    case PSBC_STAGE_LOCAL:       return sizeof(GnmLsShader);
    case PSBC_STAGE_FRAGMENT:    return sizeof(GnmPsShader);
    case PSBC_STAGE_COMPUTE:     return sizeof(GnmCsShader);
    case PSBC_STAGE_GEOMETRY:    return sizeof(GnmGsShader);
    case PSBC_STAGE_TESS_CTRL:   return sizeof(GnmHsShader);
    default:                      return sizeof(GnmVsShader);
    }
}

static uint32_t hashsb(
    const void* code, uint32_t codesize, const GnmShaderBinaryInfo* sb
) {
    static const uint8_t padbytes[8] = {0};
    const uint32_t numpadbytes =
        (sb->length & 0x7) ? 8 - (sb->length & 0x7) : 0;
    assert(numpadbytes <= sizeof(padbytes));

    uint32_t hash = crc32_sb(code, codesize, crc32_sb_begin());
    hash = crc32_sb(padbytes, numpadbytes, hash);
    hash = crc32_sb(sb, sizeof(*sb) - sizeof(sb->crc32), hash);
    return crc32_sb_end(hash);
}

/*
 * Compute the number of position exports from shader info.
 * Ported from radv_get_num_pos_exports() which is static in radv_shader.c.
 */
static unsigned get_num_pos_exports(const struct radv_shader_info *info,
                                    unsigned *clip_dist_mask_out,
                                    unsigned *cull_dist_mask_out) {
    unsigned num = 1;

    if (info->outinfo.writes_pointsize || info->outinfo.writes_viewport_index ||
        info->outinfo.writes_layer || info->outinfo.writes_primitive_shading_rate)
        num++;

    unsigned num_clip_dist_comps = util_bitcount(info->outinfo.clip_dist_mask);
    unsigned num_cull_dist_comps = info->has_ngg_culling ? 0 : util_bitcount(info->outinfo.cull_dist_mask);
    unsigned clip_cull_mask = BITFIELD_MASK(num_clip_dist_comps + num_cull_dist_comps);

    if (clip_cull_mask & 0x0f)
        num++;
    if (clip_cull_mask & 0xf0)
        num++;

    if (clip_dist_mask_out)
        *clip_dist_mask_out = BITFIELD_MASK(num_clip_dist_comps);
    if (cull_dist_mask_out)
        *cull_dist_mask_out = BITFIELD_RANGE(num_clip_dist_comps, num_cull_dist_comps);
    return num;
}

/*
 * Compute db_shader_control register value for PS.
 * Ported from the inline computation in radv_precompute_registers_hw_ps().
 */
static uint32_t compute_db_shader_control(const struct radv_shader_info *info) {
    unsigned conservative_z_export = V_02880C_EXPORT_ANY_Z;
    if (info->ps.depth_layout == FRAG_DEPTH_LAYOUT_GREATER)
        conservative_z_export = V_02880C_EXPORT_GREATER_THAN_Z;
    else if (info->ps.depth_layout == FRAG_DEPTH_LAYOUT_LESS)
        conservative_z_export = V_02880C_EXPORT_LESS_THAN_Z;

    const unsigned z_order =
        info->ps.early_fragment_test || !info->ps.writes_memory
            ? V_02880C_EARLY_Z_THEN_LATE_Z : V_02880C_LATE_Z;

    return S_02880C_Z_EXPORT_ENABLE(info->ps.writes_z) |
           S_02880C_STENCIL_TEST_VAL_EXPORT_ENABLE(info->ps.writes_stencil) |
           S_02880C_KILL_ENABLE(info->ps.can_discard) |
           S_02880C_MASK_EXPORT_ENABLE(info->ps.writes_sample_mask) |
           S_02880C_CONSERVATIVE_Z_EXPORT(conservative_z_export) |
           S_02880C_Z_ORDER(z_order) |
           S_02880C_DEPTH_BEFORE_SHADER(info->ps.early_fragment_test) |
           S_02880C_PRE_SHADER_DEPTH_COVERAGE_ENABLE(info->ps.post_depth_coverage) |
           S_02880C_EXEC_ON_HIER_FAIL(info->ps.writes_memory) |
           S_02880C_EXEC_ON_NOOP(info->ps.writes_memory) |
           S_02880C_PRIMITIVE_ORDERED_PIXEL_SHADER(info->ps.pops);
}

/* === Shader binary builder === */

typedef struct {
    bool valid;
    uint32_t count;
    uint32_t words[PSBC_MAX_SEMANTICS];
} PsbcInputSemantics;

typedef struct {
    const struct nir_shader* nir;
    const struct radv_shader_info* rinfo;
    const struct radv_shader_args* rargs;
    const struct ac_shader_config* config;
    enum amd_gfx_level gfx_level;
    enum radeon_family family;
    mesa_shader_stage stage;
    PsbcStage psbc_stage;
    const uint32_t* spirv_data;
    size_t spirv_size;
    PsbcTarget target;
    bool ngg;
    bool neo;
    uint32_t address32_hi;
    const PsbcCompileOptions* options;
    const PsbcInputSemantics* input_semantics;
} BuildContext;

static unsigned ps5_last_provoking_vertex(uint32_t primitive_type) {
    switch (primitive_type) {
    case 1: /* point list */
        return 0;
    case 2: /* line list */
    case 3: /* line strip */
        return 1;
    case 4: /* triangle list */
    case 5: /* triangle fan */
    case 6: /* triangle strip */
        return 2;
    default:
        return 0;
    }
}

static bool lower_flat_input_vertex(nir_builder* builder,
                                    nir_intrinsic_instr* intrinsic,
                                    void* data) {
    const PsbcCompileOptions* options = data;
    /* The Gallium flag carries GL's flatshade_first state despite its legacy
     * name. Select the corresponding lane explicitly on PS5. */
    const unsigned vertex_id = options->flat_input_vertex_valid ?
        options->flat_input_vertex :
        (options->provoking_vtx_last ?
            ps5_last_provoking_vertex(options->primitive_type) : 1);
    if (intrinsic->intrinsic != nir_intrinsic_load_input)
        return false;
    /* An implicit NGG PrimitiveID has one value per primitive, not P0/P1/P2.
     * Explicit GS outputs still obey the ordinary provoking-vertex rule. */
    if (options->primitive_id_per_primitive &&
        nir_intrinsic_io_semantics(intrinsic).location == VARYING_SLOT_PRIMITIVE_ID)
        return false;

    builder->cursor = nir_before_instr(&intrinsic->instr);
    nir_def* replacement = nir_load_input_vertex(
        builder, intrinsic->def.num_components, intrinsic->def.bit_size,
        nir_imm_int(builder, vertex_id), intrinsic->src[0].ssa,
        .base = nir_intrinsic_base(intrinsic),
        .component = nir_intrinsic_component(intrinsic),
        .dest_type = nir_intrinsic_dest_type(intrinsic),
        .io_semantics = nir_intrinsic_io_semantics(intrinsic));
    nir_def_replace(&intrinsic->def, replacement);
    return true;
}

static unsigned ps5_input_interpolation_mode(
    const nir_intrinsic_instr* intrinsic) {
    if (intrinsic->intrinsic == nir_intrinsic_load_interpolated_input)
        return 2;
    if (intrinsic->intrinsic == nir_intrinsic_load_input ||
        intrinsic->intrinsic == nir_intrinsic_load_input_vertex)
        return 1;
    return 0;
}

/* AGC uses only equality of producer/consumer match keys. Reserve keys
 * 49..62 for compatibility varyings, after generic 15..46 and built-ins 47/48.
 * Keep hardware parameter limits at 32; semantic namespaces are independent. */
static unsigned ps5_varying_match_key(unsigned location) {
    if (location >= VARYING_SLOT_VAR0 && location <= VARYING_SLOT_VAR31)
        return 15u + location - VARYING_SLOT_VAR0;
    if ((location >= VARYING_SLOT_COL0 && location <= VARYING_SLOT_TEX7) ||
        location == VARYING_SLOT_BFC0 || location == VARYING_SLOT_BFC1)
        return 49u + location - VARYING_SLOT_COL0;
    return UINT_MAX;
}

typedef struct {
    uint8_t modes[VARYING_SLOT_MAX];
    uint8_t flat_attribute[VARYING_SLOT_MAX];
    uint8_t primitive_id_attribute;
} Ps5MixedInputState;

static bool classify_ps5_input(nir_builder* builder,
                               nir_instr* instruction, void* data) {
    (void)builder;
    if (instruction->type != nir_instr_type_intrinsic)
        return false;
    nir_intrinsic_instr* intrinsic = nir_instr_as_intrinsic(instruction);
    const unsigned mode = ps5_input_interpolation_mode(intrinsic);
    if (!mode)
        return false;
    const nir_io_semantics io = nir_intrinsic_io_semantics(intrinsic);
    if (ps5_varying_match_key(io.location) == UINT_MAX)
        return false;
    for (unsigned slot = 0; slot < io.num_slots; ++slot) {
        const unsigned location = io.location + slot;
        if (location < VARYING_SLOT_MAX)
            ((Ps5MixedInputState*)data)->modes[location] |= mode;
    }
    return false;
}

static bool remap_ps5_flat_input(nir_builder* builder,
                                 nir_instr* instruction, void* data) {
    (void)builder;
    if (instruction->type != nir_instr_type_intrinsic)
        return false;
    nir_intrinsic_instr* intrinsic = nir_instr_as_intrinsic(instruction);
    if (ps5_input_interpolation_mode(intrinsic) != 1)
        return false;
    const nir_io_semantics io = nir_intrinsic_io_semantics(intrinsic);
    const Ps5MixedInputState* state = data;
    if (io.location == VARYING_SLOT_PRIMITIVE_ID &&
        state->primitive_id_attribute != UINT8_MAX) {
        nir_intrinsic_set_base(intrinsic, state->primitive_id_attribute);
        return true;
    }
    if (ps5_varying_match_key(io.location) == UINT_MAX || io.num_slots != 1)
        return false;
    const unsigned location = io.location;
    if (location >= VARYING_SLOT_MAX || state->modes[location] != 3)
        return false;
    nir_intrinsic_set_base(intrinsic, state->flat_attribute[location]);
    return true;
}

static bool split_ps5_mixed_inputs(nir_shader* nir,
                                   struct radv_shader_info* info,
                                   bool primitive_id_per_primitive) {
    Ps5MixedInputState state = {.primitive_id_attribute = UINT8_MAX};
    memset(state.flat_attribute, UINT8_MAX, sizeof(state.flat_attribute));
    nir_shader_instructions_pass(nir, classify_ps5_input,
                                 nir_metadata_all, &state);
    const bool per_primitive_id = primitive_id_per_primitive && info->ps.prim_id_input;
    unsigned next = info->ps.num_inputs - per_primitive_id;
    for (unsigned location = 0; location < VARYING_SLOT_MAX; ++location) {
        if (state.modes[location] != 3)
            continue;
        if (next >= PSBC_MAX_SEMANTICS)
            return false;
        state.flat_attribute[location] = next++;
    }
    /* GFX10.3 requires per-primitive inputs after every per-vertex input,
     * including the extra flat aliases created above. */
    if (per_primitive_id) {
        if (next >= PSBC_MAX_SEMANTICS)
            return false;
        state.primitive_id_attribute = next++;
    }
    if (next == info->ps.num_inputs)
        return true;
    nir_shader_instructions_pass(nir, remap_ps5_flat_input,
                                 nir_metadata_all, &state);
    info->ps.num_inputs = next;
    return true;
}

#define PSBC_CX_OFFSET(reg) ((uint16_t)(((reg) - SI_CONTEXT_REG_OFFSET) / 4))
#define PSBC_SH_OFFSET(reg) ((uint16_t)(((reg) - SI_SH_REG_OFFSET) / 4))
#define PSBC_UC_OFFSET(reg) ((uint16_t)(((reg) - CIK_UCONFIG_REG_OFFSET) / 4))

static void metadata_add_register(PsbcRegisterWrite* registers,
                                  uint32_t* count, uint32_t limit,
                                  uint16_t offset, uint32_t value) {
    if (*count >= limit)
        return;
    registers[*count] = (PsbcRegisterWrite) {
        .offset = offset,
        .value = value,
    };
    *count += 1;
}

/*
 * AGC semantic words use the low byte as the producer/consumer match key.
 * Generic user varyings start at semantic 15, matching both Sony-produced
 * AGC packages and the legacy GNM/PSSL metadata convention.
 * A producer additionally stores its parameter-export index in bits 8..12.
 * Pixel consumer bit 22 selects flat shading in PS5 AGC semantics.  This is
 * the PS5 counterpart of GnmPixelInputSemantic.isflatshaded and is consumed
 * by sceAgcLinkShaders when it builds SPI_PS_INPUT_CNTL_i.
 */
static bool build_input_semantics(const nir_shader* nir,
                                  const struct radv_shader_info* info,
                                  bool primitive_id_per_primitive,
                                  PsbcInputSemantics* result) {
    memset(result, 0, sizeof(*result));
    const uint32_t generic_count = info->ps.num_inputs;
    if (generic_count > PSBC_MAX_SEMANTICS)
        return false;
    if (info->ps.input_per_primitive_mask ||
        info->ps.explicit_shaded_mask || info->ps.explicit_strict_shaded_mask ||
        info->ps.float16_shaded_mask || info->ps.float16_hi_shaded_mask)
        return false;

    uint32_t words_by_attribute[PSBC_MAX_SEMANTICS] = {0};
    bool seen[PSBC_MAX_SEMANTICS] = {false};
    nir_foreach_function_impl(impl, nir) {
        nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
                if (instr->type != nir_instr_type_intrinsic)
                    continue;
                const nir_intrinsic_instr* intrin = nir_instr_as_intrinsic(instr);
                const unsigned mode = ps5_input_interpolation_mode(intrin);
                if (!mode)
                    continue;
                const nir_io_semantics io = nir_intrinsic_io_semantics(intrin);
                const bool primitive_id = io.location == VARYING_SLOT_PRIMITIVE_ID;
                const bool point_coord = io.location == VARYING_SLOT_PNTC;
                if (primitive_id && (io.num_slots != 1 || mode != 1))
                    return false;
                if (point_coord && (io.num_slots != 1 || mode != 2))
                    return false;
                if (!primitive_id && !point_coord && ps5_varying_match_key(io.location) == UINT_MAX)
                    continue;
                for (uint32_t slot = 0; slot < io.num_slots; ++slot) {
                    const uint32_t location =
                        (primitive_id || point_coord) ? 0 : ps5_varying_match_key(io.location + slot);
                    const uint32_t attribute =
                        nir_intrinsic_base(intrin) + slot;
                    if (location == UINT_MAX ||
                        attribute >= generic_count)
                        return false;
                    uint32_t word = point_coord ? PSBC_SEMANTIC_POINT_COORD :
                        primitive_id ? PSBC_SEMANTIC_PRIMITIVE_ID : location;
                    if (mode == 1 && !(primitive_id && primitive_id_per_primitive))
                        word |= BITFIELD_BIT(22);
                    if (seen[attribute] &&
                        words_by_attribute[attribute] != word)
                        return false;
                    words_by_attribute[attribute] = word;
                    seen[attribute] = true;
                }
            }
        }
    }

    for (uint32_t attribute = 0; attribute < generic_count; ++attribute) {
        if (!seen[attribute])
            return false;
        result->words[result->count++] = words_by_attribute[attribute];
    }
    result->valid = true;
    return true;
}

static bool fill_output_semantics(const struct radv_shader_info* info,
                                  PsbcShaderMetadata* metadata) {
    for (unsigned semantic = 0; semantic < VARYING_SLOT_MAX; ++semantic) {
        const unsigned key = ps5_varying_match_key(semantic);
        if (key == UINT_MAX)
            continue;
        const uint8_t parameter =
            info->outinfo.vs_output_param_offset[semantic];
        if (parameter < PSBC_MAX_SEMANTICS) {
            if (metadata->output_semantic_count >= PSBC_MAX_SEMANTICS)
                return false;
            metadata->output_semantics[metadata->output_semantic_count++] =
                key | ((uint32_t)parameter << 8);
        }
    }
    const uint8_t primitive_id =
        info->outinfo.vs_output_param_offset[VARYING_SLOT_PRIMITIVE_ID];
    if (primitive_id < PSBC_MAX_SEMANTICS) {
        if (metadata->output_semantic_count >= PSBC_MAX_SEMANTICS)
            return false;
        metadata->output_semantics[metadata->output_semantic_count++] =
            PSBC_SEMANTIC_PRIMITIVE_ID | ((uint32_t)primitive_id << 8);
    }
    return metadata->output_semantic_count ==
        info->outinfo.param_exports + info->outinfo.prim_param_exports;
}

static uint32_t build_pa_cl_vs_out_cntl(const BuildContext* ctx,
                                        unsigned num_pos_exports) {
    unsigned clip_dist_mask = 0, cull_dist_mask = 0;
    get_num_pos_exports(ctx->rinfo, &clip_dist_mask, &cull_dist_mask);
    const uint32_t total_mask = clip_dist_mask | cull_dist_mask;
    const bool misc_vec_ena =
        ctx->rinfo->outinfo.writes_pointsize ||
        ctx->rinfo->outinfo.writes_layer ||
        ctx->rinfo->outinfo.writes_viewport_index ||
        ctx->rinfo->outinfo.writes_primitive_shading_rate;

    return S_02881C_USE_VTX_POINT_SIZE(ctx->rinfo->outinfo.writes_pointsize) |
           S_02881C_USE_VTX_RENDER_TARGET_INDX(ctx->rinfo->outinfo.writes_layer) |
           S_02881C_USE_VTX_VIEWPORT_INDX(ctx->rinfo->outinfo.writes_viewport_index) |
           S_02881C_USE_VTX_VRS_RATE(ctx->rinfo->outinfo.writes_primitive_shading_rate) |
           S_02881C_VS_OUT_MISC_VEC_ENA(misc_vec_ena) |
           S_02881C_VS_OUT_MISC_SIDE_BUS_ENA(
               misc_vec_ena || (ctx->gfx_level >= GFX10_3 && num_pos_exports > 1)) |
           S_02881C_VS_OUT_CCDIST0_VEC_ENA((total_mask & 0x0f) != 0) |
           S_02881C_VS_OUT_CCDIST1_VEC_ENA((total_mask & 0xf0) != 0) |
           total_mask << 8 | clip_dist_mask;
}

static uint32_t build_spi_shader_pos_format(unsigned num_pos_exports) {
    return S_02870C_POS0_EXPORT_FORMAT(V_02870C_SPI_SHADER_4COMP) |
           S_02870C_POS1_EXPORT_FORMAT(num_pos_exports > 1
                                          ? V_02870C_SPI_SHADER_4COMP
                                          : V_02870C_SPI_SHADER_NONE) |
           S_02870C_POS2_EXPORT_FORMAT(num_pos_exports > 2
                                          ? V_02870C_SPI_SHADER_4COMP
                                          : V_02870C_SPI_SHADER_NONE) |
           S_02870C_POS3_EXPORT_FORMAT(num_pos_exports > 3
                                          ? V_02870C_SPI_SHADER_4COMP
                                          : V_02870C_SPI_SHADER_NONE);
}

static void fill_shader_metadata(const BuildContext* ctx,
                                 PsbcShaderMetadata* metadata) {
    const bool has_vertex_inputs =
        ctx->stage == MESA_SHADER_VERTEX ||
        ctx->stage == MESA_SHADER_TESS_CTRL ||
        (ctx->stage == MESA_SHADER_GEOMETRY && ctx->ngg);

    memset(metadata, 0, sizeof(*metadata));
    metadata->version = PSBC_SHADER_METADATA_VERSION;
    metadata->target = ctx->target;
    metadata->source_stage = ctx->psbc_stage;
    metadata->unresolved_fields = PSBC_UNRESOLVED_PROGRAM_CHECKSUM;
    metadata->clip_distance_mask = ctx->rinfo->outinfo.clip_dist_mask;
    metadata->cull_distance_mask = ctx->rinfo->outinfo.cull_dist_mask;
    metadata->address32_hi = ctx->address32_hi;
    metadata->user_sgpr_count = ctx->rargs->num_user_sgprs;
    if (ctx->ngg && ctx->rargs->ngg_lds_layout.used) {
        metadata->ngg_lds_layout_valid = true;
        metadata->ngg_lds_layout_user_data_dword =
            ctx->rargs->user_sgprs_locs.shader_data[AC_UD_NGG_LDS_LAYOUT].sgpr_idx;
        metadata->ngg_lds_layout = ctx->rinfo->ngg_info.esgs_ring_size;
    }
    if (ctx->config->scratch_bytes_per_wave) {
        metadata->scratch_valid = true;
        metadata->scratch_buffer_backed = ctx->options->compute_buffer_spills;
        metadata->scratch_bytes_per_wave =
            ctx->config->scratch_bytes_per_wave;
        metadata->scratch_size_per_thread = ctx->nir->scratch_size;
        metadata->scratch_buffer_table_user_data_dword =
            ctx->rargs->user_sgprs_locs
                .shader_data[AC_UD_SCRATCH_RING_OFFSETS].sgpr_idx;
    }
    if (ctx->rargs->descriptors[0].used &&
        ctx->rargs->user_sgprs_locs.descriptor_sets[0].sgpr_idx !=
            UINT32_MAX) {
        metadata->descriptor_set0_valid = true;
        metadata->descriptor_set0_user_data_dword =
            ctx->rargs->user_sgprs_locs.descriptor_sets[0].sgpr_idx;
    }
    if (has_vertex_inputs && ctx->rargs->ac.vertex_buffers.used) {
        metadata->vertex_buffer_table_valid = true;
        metadata->vertex_buffer_table_user_data_dword =
            ctx->rargs->user_sgprs_locs
                .shader_data[AC_UD_VS_VERTEX_BUFFERS].sgpr_idx;
    }
    if (has_vertex_inputs && ctx->rargs->ac.base_vertex.used) {
        metadata->base_vertex_valid = true;
        metadata->base_vertex_user_data_dword =
            ctx->rargs->user_sgprs_locs
                .shader_data[AC_UD_VS_BASE_VERTEX_START_INSTANCE].sgpr_idx;
    }
    if (has_vertex_inputs && ctx->rargs->ac.is_indexed_draw.used) {
        const struct ac_shader_args* args = &ctx->rargs->ac;
        metadata->is_indexed_draw_valid = true;
        metadata->is_indexed_draw_user_data_dword =
            metadata->base_vertex_user_data_dword +
            args->args[args->is_indexed_draw.arg_index].offset -
            args->args[args->base_vertex.arg_index].offset;
    }
    if (has_vertex_inputs && ctx->rargs->ac.start_instance.used) {
        const struct ac_shader_args* args = &ctx->rargs->ac;
        metadata->start_instance_valid = true;
        metadata->start_instance_user_data_dword =
            metadata->base_vertex_user_data_dword +
            args->args[args->start_instance.arg_index].offset -
            args->args[args->base_vertex.arg_index].offset;
    }
    if (has_vertex_inputs && ctx->rargs->instance_id_bias.used) {
        const struct ac_shader_args *args = &ctx->rargs->ac;
        metadata->instance_id_bias_valid = true;
        metadata->instance_id_bias_user_data_dword =
            metadata->base_vertex_user_data_dword +
            args->args[ctx->rargs->instance_id_bias.arg_index].offset -
            args->args[args->base_vertex.arg_index].offset;
    }
    if (ctx->ngg && ctx->rargs->ngg_query_buf_va.used && ctx->rargs->ngg_state.used) {
        metadata->primitive_query_valid = true;
        metadata->primitive_query_buffer_user_data_dword = ctx->rargs->user_sgprs_locs
            .shader_data[AC_UD_NGG_QUERY_BUF_VA].sgpr_idx;
        metadata->primitive_query_state_user_data_dword = ctx->rargs->user_sgprs_locs
            .shader_data[AC_UD_NGG_STATE].sgpr_idx;
        metadata->primitive_query_enable_mask = radv_shader_query_prim_gen << NGG_STATE_QUERY__SHIFT;
        metadata->primitive_query_counter_offset = RADV_SHADER_QUERY_PRIM_GEN_OFFSET(0);
    }
    if (ctx->rinfo->so.enabled_stream_buffers_mask &&
        ctx->rargs->streamout_buffers.used &&
        (ctx->ngg || (ctx->rargs->ac.streamout_config.used &&
                      ctx->rargs->ac.streamout_write_index.used))) {
        const struct ac_shader_args* args = &ctx->rargs->ac;
        metadata->streamout_valid = true;
        metadata->streamout_buffer_table_user_data_dword =
            ctx->rargs->user_sgprs_locs
                .shader_data[AC_UD_STREAMOUT_BUFFERS].sgpr_idx;
        metadata->streamout_enabled_stream_buffers_mask =
            ctx->rinfo->so.enabled_stream_buffers_mask;
        if (!ctx->ngg) {
            metadata->streamout_config_sgpr =
                args->args[args->streamout_config.arg_index].offset;
            metadata->streamout_write_index_sgpr =
                args->args[args->streamout_write_index.arg_index].offset;
        }
        for (unsigned i = 0; i < 4; ++i) {
            metadata->streamout_strides_dwords[i] =
                ctx->rinfo->so.strides[i];
            if (!ctx->ngg && args->streamout_offset[i].used)
                metadata->streamout_offset_sgprs[i] =
                    args->args[args->streamout_offset[i].arg_index].offset;
        }
    }
    metadata->descriptor_binding_count =
        ctx->options->descriptor_binding_count;
    memcpy(metadata->descriptor_bindings, ctx->options->descriptor_bindings,
           metadata->descriptor_binding_count *
               sizeof(metadata->descriptor_bindings[0]));

    PsbcRegisterWrite* cx = metadata->context_registers;
    PsbcRegisterWrite* sh = metadata->shader_registers;
    uint32_t* cx_count = &metadata->context_register_count;
    uint32_t* sh_count = &metadata->shader_register_count;

    if (ctx->stage == MESA_SHADER_COMPUTE) {
        metadata->hardware_stage = PSBC_HW_STAGE_COMPUTE;
        metadata->compute_wave_size = ctx->rinfo->wave_size;
        metadata->compute_lds_bytes = ac_shader_encode_lds_size(
            ctx->config->lds_size, ctx->gfx_level, ctx->stage) * 512u;
        for (unsigned i = 0; i < 3; ++i)
            metadata->compute_workgroup_size[i] = ctx->rinfo->cs.block_size[i];
        if (ctx->rargs->ac.num_work_groups.used) {
            metadata->compute_grid_size_valid = true;
            metadata->compute_grid_size_user_data_dword =
                ctx->rargs->user_sgprs_locs.shader_data[AC_UD_CS_GRID_SIZE].sgpr_idx;
        }
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B830_COMPUTE_PGM_LO), 0);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B834_COMPUTE_PGM_HI), 0);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B848_COMPUTE_PGM_RSRC1), ctx->config->rsrc1);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B84C_COMPUTE_PGM_RSRC2), ctx->config->rsrc2);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B8A0_COMPUTE_PGM_RSRC3), ctx->config->rsrc3);
        for (unsigned i = 0; i < 3; ++i)
            metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
                PSBC_SH_OFFSET(R_00B81C_COMPUTE_NUM_THREAD_X) + i,
                metadata->compute_workgroup_size[i]);
        return;
    }

    if (ctx->stage == MESA_SHADER_FRAGMENT) {
        const unsigned per_primitive_inputs = ctx->target == PSBC_TARGET_PS5 &&
            ctx->options->primitive_id_per_primitive && ctx->rinfo->ps.prim_id_input;
        const bool param_gen = ctx->gfx_level >= GFX11 &&
                               !ctx->rinfo->ps.num_inputs &&
                               ctx->config->lds_size;
        metadata->hardware_stage = PSBC_HW_STAGE_PIXEL;
        if (!ctx->input_semantics || !ctx->input_semantics->valid)
            metadata->unresolved_fields |= PSBC_UNRESOLVED_AGC_LINKAGE;
        else {
            metadata->input_semantic_count = ctx->input_semantics->count;
            memcpy(metadata->input_semantics, ctx->input_semantics->words,
                   metadata->input_semantic_count *
                       sizeof(metadata->input_semantics[0]));
        }
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_028710_SPI_SHADER_Z_FORMAT),
            ac_get_spi_shader_z_format(ctx->rinfo->ps.writes_z,
                ctx->rinfo->ps.writes_stencil,
                ctx->rinfo->ps.writes_sample_mask,
                ctx->rinfo->ps.writes_mrt0_alpha));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_028714_SPI_SHADER_COL_FORMAT),
            compact_spi_shader_col_format(ctx->rinfo->ps.spi_shader_col_format));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_0286CC_SPI_PS_INPUT_ENA),
            ctx->config->spi_ps_input_ena);
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_0286D0_SPI_PS_INPUT_ADDR),
            ctx->config->spi_ps_input_addr);
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_0286D8_SPI_PS_IN_CONTROL),
            /* RADV normally leaves NUM_INTERP for its GFX10.3 pipeline linker
             * because per-primitive inputs use a separate count.  AGC's
             * standalone linker does not fill the per-vertex count for our
             * generated package, so carry the compiler-proven count here. */
            S_0286D8_NUM_INTERP(ctx->rinfo->ps.num_inputs - per_primitive_inputs) |
            S_0286D8_NUM_PRIM_INTERP(per_primitive_inputs) |
            S_0286D8_PS_W32_EN(ctx->rinfo->wave_size == 32) |
            S_0286D8_PARAM_GEN(param_gen));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_0286E0_SPI_BARYC_CNTL),
            S_0286E0_FRONT_FACE_ALL_BITS(0));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_02880C_DB_SHADER_CONTROL),
            compute_db_shader_control(ctx->rinfo));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_02823C_CB_SHADER_MASK),
            ac_get_cb_shader_mask(ctx->rinfo->ps.spi_shader_col_format));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_028C40_PA_SC_SHADER_CONTROL), 0);

        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B020_SPI_SHADER_PGM_LO_PS), 0);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B024_SPI_SHADER_PGM_HI_PS), 0);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B028_SPI_SHADER_PGM_RSRC1_PS),
            ctx->config->rsrc1);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B02C_SPI_SHADER_PGM_RSRC2_PS),
            ctx->config->rsrc2);
        return;
    }

    if (ctx->stage == MESA_SHADER_TESS_CTRL) {
        /* GFX10 executes merged VS+TCS code in the hull hardware stage.
         * PGM_HI is initialized by the graphics preamble and GFX10 RADV
         * programs only PGM_LO_LS plus PGM_RSRC1_HS here. */
        metadata->hardware_stage = PSBC_HW_STAGE_HULL;
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B520_SPI_SHADER_PGM_LO_LS), 0);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B428_SPI_SHADER_PGM_RSRC1_HS),
            ctx->config->rsrc1);
        return;
    }

    if ((ctx->stage == MESA_SHADER_VERTEX ||
         ctx->stage == MESA_SHADER_TESS_EVAL ||
         ctx->stage == MESA_SHADER_GEOMETRY) && ctx->ngg) {
        const bool has_geometry = ctx->stage == MESA_SHADER_GEOMETRY;
        const bool has_tessellation = ctx->stage == MESA_SHADER_TESS_EVAL;
        const uint32_t nparams = MAX2(ctx->rinfo->outinfo.param_exports, 1);
        const bool no_pc_export = ctx->rinfo->outinfo.param_exports == 0 &&
                                  ctx->rinfo->outinfo.prim_param_exports == 0;
        const unsigned num_prim_params = ctx->rinfo->outinfo.prim_param_exports;
        const unsigned num_pos_exports = get_num_pos_exports(ctx->rinfo, NULL, NULL);
        const uint32_t gs_num_invocations =
            ctx->rinfo->stage == MESA_SHADER_GEOMETRY
                ? ctx->rinfo->gs.invocations : 1;

        metadata->hardware_stage = PSBC_HW_STAGE_NGG;
        metadata->unresolved_fields |=
            PSBC_UNRESOLVED_NGG_ESGS_RING_ITEMSIZE;
        if (!fill_output_semantics(ctx->rinfo, metadata))
            metadata->unresolved_fields |= PSBC_UNRESOLVED_AGC_LINKAGE;

        metadata->linkage_valid = true;
        metadata->linkage_ge_cntl = (PsbcRegisterWrite) {
            .offset = PSBC_UC_OFFSET(R_03096C_GE_CNTL),
            .value = S_03096C_PRIM_GRP_SIZE_GFX10(
                         ctx->rinfo->ngg_info.max_gsprims) |
                     S_03096C_VERT_GRP_SIZE(
                         has_tessellation ? 0 :
                         ctx->rinfo->ngg_info.hw_max_esverts),
        };
        metadata->linkage_stages_en = (PsbcRegisterWrite) {
            .offset = PSBC_CX_OFFSET(R_028B54_VGT_SHADER_STAGES_EN),
            /* Match RADV's GFX10 VGT shader-stage programming.  NGG runs
             * through the ES/GS hardware path even without an API geometry
             * shader.  A real GS must enable GS and cannot use NGG
             * passthrough; otherwise its emitted primitives are skipped. */
            .value = S_028B54_LS_EN(has_tessellation ? V_028B54_LS_STAGE_ON : 0) |
                     S_028B54_HS_EN(has_tessellation) |
                     S_028B54_DYNAMIC_HS(has_tessellation) |
                     S_028B54_ES_EN(has_tessellation ? V_028B54_ES_STAGE_DS :
                                                     V_028B54_ES_STAGE_REAL) |
                     S_028B54_GS_EN(has_geometry) |
                     S_028B54_PRIMGEN_EN(1) |
                     S_028B54_MAX_PRIMGRP_IN_WAVE(2) |
                     S_028B54_GS_W32_EN(ctx->rinfo->wave_size == 32) |
                     S_028B54_VS_W32_EN(ctx->stage == MESA_SHADER_VERTEX &&
                                        ctx->rinfo->wave_size == 32) |
                     S_028B54_NGG_WAVE_ID_EN(
                         ctx->rinfo->ngg_wave_id_en) |
                     S_028B54_PRIMGEN_PASSTHRU_EN(
                         ctx->rinfo->is_ngg_passthrough),
        };
        metadata->linkage_user_vgpr_en = (PsbcRegisterWrite) {
            .offset = PSBC_UC_OFFSET(R_030988_GE_USER_VGPR_EN),
            .value = 0,
        };
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_0286C4_SPI_VS_OUT_CONFIG),
            S_0286C4_VS_EXPORT_COUNT(nparams - 1) |
            S_0286C4_PRIM_EXPORT_COUNT(num_prim_params) |
            S_0286C4_NO_PC_EXPORT(no_pc_export));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_028A84_VGT_PRIMITIVEID_EN),
            S_028A84_NGG_DISABLE_PROVOK_REUSE(
                ctx->stage == MESA_SHADER_VERTEX && ctx->rinfo->outinfo.export_prim_id));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_02870C_SPI_SHADER_POS_FORMAT),
            build_spi_shader_pos_format(num_pos_exports));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_028708_SPI_SHADER_IDX_FORMAT),
            S_028708_IDX0_EXPORT_FORMAT(V_028708_SPI_SHADER_1COMP));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_02881C_PA_CL_VS_OUT_CNTL),
            build_pa_cl_vs_out_cntl(ctx, num_pos_exports));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_028B4C_GE_NGG_SUBGRP_CNTL),
            S_028B4C_PRIM_AMP_FACTOR(ctx->rinfo->ngg_info.prim_amp_factor) |
            S_028B4C_THDS_PER_SUBGRP(0));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_028B90_VGT_GS_INSTANCE_CNT),
            S_028B90_CNT(gs_num_invocations) |
            S_028B90_ENABLE(gs_num_invocations > 1) |
            S_028B90_EN_MAX_VERT_OUT_PER_GS_INSTANCE(
                ctx->rinfo->ngg_info.max_vert_out_per_gs_instance));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_028A44_VGT_GS_ONCHIP_CNTL),
            S_028A44_ES_VERTS_PER_SUBGRP(ctx->rinfo->ngg_info.hw_max_esverts) |
            S_028A44_GS_PRIMS_PER_SUBGRP(ctx->rinfo->ngg_info.max_gsprims) |
            S_028A44_GS_INST_PRIMS_IN_SUBGRP(
                ctx->rinfo->ngg_info.max_gsprims * gs_num_invocations));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_0287FC_GE_MAX_OUTPUT_PER_SUBGROUP),
            S_0287FC_MAX_VERTS_PER_SUBGROUP(
                ctx->rinfo->ngg_info.max_out_verts));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_028AAC_VGT_ESGS_RING_ITEMSIZE),
            ctx->rinfo->ngg_info.vgt_esgs_ring_itemsize);
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_028B38_VGT_GS_MAX_VERT_OUT),
            ctx->rinfo->gs.vertices_out);
        if (has_geometry || has_tessellation) {
            uint32_t output_primitive = V_028A6C_TRISTRIP;

            if ((has_geometry && ctx->rinfo->gs.output_prim == MESA_PRIM_POINTS) ||
                (has_tessellation && ctx->nir->info.tess.point_mode))
                output_primitive = V_028A6C_POINTLIST;
            else if ((has_geometry && ctx->rinfo->gs.output_prim == MESA_PRIM_LINE_STRIP) ||
                     (has_tessellation &&
                      ctx->nir->info.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES))
                output_primitive = V_028A6C_LINESTRIP;

            metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
                PSBC_CX_OFFSET(R_028A6C_VGT_GS_OUT_PRIM_TYPE),
                S_028A6C_OUTPRIM_TYPE(output_primitive));
        }

        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B320_SPI_SHADER_PGM_LO_ES), 0);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B324_SPI_SHADER_PGM_HI_ES), 0);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B228_SPI_SHADER_PGM_RSRC1_GS),
            ctx->config->rsrc1);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B22C_SPI_SHADER_PGM_RSRC2_GS),
            ctx->config->rsrc2);
        if (ctx->target == PSBC_TARGET_PS5) {
            /* GFX10 programs these for every NGG shader.  Keep late
             * allocation disabled until the console CU topology is known;
             * this is the conservative ac_compute_late_alloc fallback. */
            metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
                PSBC_SH_OFFSET(R_00B21C_SPI_SHADER_PGM_RSRC3_GS),
                S_00B21C_CU_EN(0xffff) | S_00B21C_WAVE_LIMIT(0x3f));
            metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
                PSBC_SH_OFFSET(R_00B204_SPI_SHADER_PGM_RSRC4_GS),
                S_00B204_CU_EN_GFX10(0xffff) |
                S_00B204_SPI_SHADER_LATE_ALLOC_GS_GFX10(0));
        }
        return;
    }

    if (ctx->stage == MESA_SHADER_GEOMETRY) {
        /* A standalone legacy GS proves NIR/ACO support, but it is not a
         * complete GFX9+ pipeline: the pre-raster stage must be merged and
         * the legacy path also needs a copy shader.  Do not mislabel this
         * diagnostic artifact as a directly loadable vertex package. */
        metadata->hardware_stage = PSBC_HW_STAGE_UNKNOWN;
        metadata->unresolved_fields |= PSBC_UNRESOLVED_AGC_LINKAGE;
        return;
    }

    if (ctx->stage == MESA_SHADER_VERTEX) {
        const uint32_t nparams = MAX2(ctx->rinfo->outinfo.param_exports, 1);
        const unsigned num_pos_exports =
            get_num_pos_exports(ctx->rinfo, NULL, NULL);

        metadata->hardware_stage = PSBC_HW_STAGE_VERTEX;
        metadata->linkage_valid = true;
        metadata->linkage_ge_cntl = (PsbcRegisterWrite) {
            .offset = PSBC_UC_OFFSET(R_03096C_GE_CNTL),
            .value = S_03096C_PRIM_GRP_SIZE_GFX10(128) |
                     S_03096C_VERT_GRP_SIZE(256),
        };
        metadata->linkage_stages_en = (PsbcRegisterWrite) {
            .offset = PSBC_CX_OFFSET(R_028B54_VGT_SHADER_STAGES_EN),
            .value = S_028B54_MAX_PRIMGRP_IN_WAVE(2) |
                     S_028B54_VS_W32_EN(ctx->rinfo->wave_size == 32),
        };
        metadata->linkage_user_vgpr_en = (PsbcRegisterWrite) {
            .offset = PSBC_UC_OFFSET(R_030988_GE_USER_VGPR_EN),
            .value = 0,
        };

        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_0286C4_SPI_VS_OUT_CONFIG),
            S_0286C4_VS_EXPORT_COUNT(nparams - 1) |
            S_0286C4_NO_PC_EXPORT(ctx->rinfo->outinfo.param_exports == 0));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_02870C_SPI_SHADER_POS_FORMAT),
            build_spi_shader_pos_format(num_pos_exports));
        metadata_add_register(cx, cx_count, PSBC_MAX_CONTEXT_REGISTERS,
            PSBC_CX_OFFSET(R_02881C_PA_CL_VS_OUT_CNTL),
            build_pa_cl_vs_out_cntl(ctx, num_pos_exports));

        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B120_SPI_SHADER_PGM_LO_VS), 0);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B124_SPI_SHADER_PGM_HI_VS), 0);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B128_SPI_SHADER_PGM_RSRC1_VS),
            ctx->config->rsrc1);
        metadata_add_register(sh, sh_count, PSBC_MAX_SHADER_REGISTERS,
            PSBC_SH_OFFSET(R_00B12C_SPI_SHADER_PGM_RSRC2_VS),
            ctx->config->rsrc2);
        return;
    }
}

/* Build the shader binary into a memory buffer instead of a file. */
static PsbcResult buildshaderbinary(
    const BuildContext* ctx,
    const uint32_t* code, uint32_t code_dw,
    uint8_t** out_data, size_t* out_size
) {
    uint32_t codesize = code_dw * sizeof(uint32_t);
    if (ctx->nir->info.stage == MESA_SHADER_VERTEX &&
        ctx->rinfo->vs.has_prolog) {
        codesize += sizeof(uint32_t);
    }

    uint32_t* newcode = malloc(codesize);
    if (!newcode)
        return PSBC_RESULT_OUT_OF_MEMORY;
    uint32_t* nextcode = newcode;

    /* prepend vertex shaders that use vertex input with essentially a
     * jump instruction to a fetch "shader" */
    if (ctx->nir->info.stage == MESA_SHADER_VERTEX &&
        ctx->rinfo->vs.has_prolog) {
        /* s_swappc_b64 s[0:1], s[0:1] */
        newcode[0] = 0xbe802100;
        nextcode += 1;
    }

    memcpy(nextcode, code, code_dw * sizeof(uint32_t));

    /* Count input usage slots.
     * prolog_inputs (SUBPTR_FETCHSHADER) and vertex_buffers
     * (PTR_VERTEXBUFFERTABLE) are VS-specific — they belong to the VS
     * portion of a merged pipeline. For standalone HS/GS compilation,
     * these args are declared because previous_stage=VERTEX, but the
     * slots should not be emitted in the HS/GS shader binary.
     * The descriptor table (PTR_INDIRECTRESOURCETABLE) is shared by
     * all stages and should always be emitted when used. */
    const bool is_vs_family =
        ctx->stage == MESA_SHADER_VERTEX ||
        ctx->psbc_stage == PSBC_STAGE_EXPORT ||
        ctx->psbc_stage == PSBC_STAGE_LOCAL;

    uint32_t numinputslots = 0;
    if (is_vs_family && ctx->rargs->prolog_inputs.used) {
        numinputslots += 1;
    }
    if (is_vs_family && ctx->rargs->ac.vertex_buffers.used) {
        numinputslots += 1;
    }
    if (ctx->rargs->descriptors[0].used) {
        numinputslots += 1;
    }

    uint32_t shspecificsize = headershsize(ctx->psbc_stage);
    shspecificsize += numinputslots * sizeof(GnmInputUsageSlot);

    /* exclude position as it doesn't count as an export semantic in PSB */
    const uint32_t outputswritten =
        util_bitcount64(ctx->nir->info.outputs_written & ~VARYING_BIT_POS);

    switch (ctx->stage) {
    case MESA_SHADER_VERTEX:
    case MESA_SHADER_TESS_EVAL:  /* DS outputs vertices like VS */
        shspecificsize += util_bitcount64(ctx->nir->info.inputs_read) *
                          sizeof(GnmVertexInputSemantic);
        shspecificsize +=
            outputswritten * sizeof(GnmVertexExportSemantic);
        break;
    case MESA_SHADER_FRAGMENT:
        shspecificsize += ctx->input_semantics->count *
                          sizeof(GnmPixelInputSemantic);
        break;
    case MESA_SHADER_GEOMETRY:
        /* GS has input/export semantics like VS */
        shspecificsize += util_bitcount64(ctx->nir->info.inputs_read) *
                          sizeof(GnmVertexInputSemantic);
        shspecificsize +=
            outputswritten * sizeof(GnmVertexExportSemantic);
        break;
    case MESA_SHADER_TESS_CTRL:
        /* HS has input semantics (from VS outputs) but no export semantics */
        shspecificsize += util_bitcount64(ctx->nir->info.inputs_read) *
                          sizeof(GnmVertexInputSemantic);
        break;
    default:
        break;
    }

    /* GCN bytecode that comes after the headers must be 4 byte aligned */
    const uint32_t alignbytes =
        ((shspecificsize + 3) & (-4)) - shspecificsize;
    shspecificsize += alignbytes;

    /* Calculate total size */
    const size_t total_size = sizeof(PsslBinaryHeader) +
                              sizeof(GnmShaderFileHeader) +
                              shspecificsize +
                              codesize +
                              sizeof(GnmShaderBinaryInfo) +
                              sizeof(PsslBinaryParamInfo);

    uint8_t* buf = malloc(total_size);
    if (!buf) {
        free(newcode);
        return PSBC_RESULT_OUT_OF_MEMORY;
    }
    size_t offset = 0;

    /* PSSL binary header */
    const PsslBinaryHeader psbh = {
        .vermajor = 0,
        .verminor = 4,
        .shadertype = psbshtype(ctx->psbc_stage),
        .codetype = PSSL_CODE_ISA,
        .compilertype = PSSL_COMPILER_UNSPECIFIED,
        .codesize = sizeof(GnmShaderFileHeader) + shspecificsize +
                    codesize + sizeof(GnmShaderBinaryInfo),
    };
    memcpy(buf + offset, &psbh, sizeof(psbh));
    offset += sizeof(psbh);

    /* GNM shader file header */
    const GnmShaderFileHeader gsfh = {
        .magic = GNM_SHADER_FILE_HEADER_ID,
        .vermajor = 7,
        .verminor = 2,
        .type = gnmshtype(ctx->psbc_stage),
        .headersizedwords = shspecificsize / 4,
        .targetgpumodes =
            (ctx->gfx_level >= GFX10_3) ? GNM_TARGETGPUMODE_NEO :
            (ctx->neo ? GNM_TARGETGPUMODE_NEO : GNM_TARGETGPUMODE_BASE),
    };
    memcpy(buf + offset, &gsfh, sizeof(gsfh));
    offset += sizeof(gsfh);

    /* Shader-specific header (VS/PS/GS/HS/ES/LS registers) */
    switch (ctx->psbc_stage) {
    case PSBC_STAGE_VERTEX:
    case PSBC_STAGE_TESS_EVAL: {
        const uint32_t nparams =
            MAX2(ctx->rinfo->outinfo.param_exports, 1);
        unsigned clip_dist_mask = 0, cull_dist_mask = 0;
        const unsigned num_pos_exports =
            get_num_pos_exports(ctx->rinfo, &clip_dist_mask, &cull_dist_mask);
        const uint32_t total_mask = clip_dist_mask | cull_dist_mask;
        const bool misc_vec_ena =
            ctx->rinfo->outinfo.writes_pointsize ||
            ctx->rinfo->outinfo.writes_layer ||
            ctx->rinfo->outinfo.writes_viewport_index ||
            ctx->rinfo->outinfo.writes_primitive_shading_rate;

        const GnmVsShader vsh = {
            .common =
                {
                    .shadersize =
                        codesize + sizeof(GnmShaderBinaryInfo),
                    .numinputusageslots = numinputslots,
                },
            .registers =
                {
                    .spishaderpgmlovs = shspecificsize,
                    .spishaderpgmhivs = 0xffffffff,
                    .spishaderpgmrsrc1vs = ctx->config->rsrc1,
                    .spishaderpgmrsrc2vs = ctx->config->rsrc2,
                    .spivsoutconfig =
                        S_0286C4_VS_EXPORT_COUNT(nparams - 1),
                    .spishaderposformat =
                        S_02870C_POS0_EXPORT_FORMAT(
                            V_02870C_SPI_SHADER_4COMP
                        ) |
                        S_02870C_POS1_EXPORT_FORMAT(
                            num_pos_exports > 1
                                ? V_02870C_SPI_SHADER_4COMP
                                : V_02870C_SPI_SHADER_NONE
                        ) |
                        S_02870C_POS2_EXPORT_FORMAT(
                            num_pos_exports > 2
                                ? V_02870C_SPI_SHADER_4COMP
                                : V_02870C_SPI_SHADER_NONE
                        ) |
                        S_02870C_POS3_EXPORT_FORMAT(
                            num_pos_exports > 3
                                ? V_02870C_SPI_SHADER_4COMP
                                : V_02870C_SPI_SHADER_NONE
                        ),
                    .paclvsoutcntl =
                        S_02881C_USE_VTX_POINT_SIZE(
                            ctx->rinfo->outinfo.writes_pointsize
                        ) |
                        S_02881C_USE_VTX_RENDER_TARGET_INDX(
                            ctx->rinfo->outinfo.writes_layer
                        ) |
                        S_02881C_USE_VTX_VIEWPORT_INDX(
                            ctx->rinfo->outinfo.writes_viewport_index
                        ) |
                        S_02881C_USE_VTX_VRS_RATE(
                            ctx->rinfo->outinfo
                                .writes_primitive_shading_rate
                        ) |
                        S_02881C_VS_OUT_MISC_VEC_ENA(misc_vec_ena) |
                        S_02881C_VS_OUT_MISC_SIDE_BUS_ENA(
                            misc_vec_ena ||
                            (ctx->gfx_level >= GFX10_3 &&
                             num_pos_exports > 1)
                        ) |
                        S_02881C_VS_OUT_CCDIST0_VEC_ENA(
                            (total_mask & 0x0f) != 0
                        ) |
                        S_02881C_VS_OUT_CCDIST1_VEC_ENA(
                            (total_mask & 0xf0) != 0
                        ) |
                        total_mask << 8 |
                        clip_dist_mask,
                },
            .numinputsemantics =
                ctx->input_semantics->count,
            .numexportsemantics = outputswritten,
        };
        memcpy(buf + offset, &vsh, sizeof(vsh));
        offset += sizeof(vsh);
        break;
    }
    case PSBC_STAGE_FRAGMENT: {
        const bool param_gen = ctx->gfx_level >= GFX11 &&
                               !ctx->rinfo->ps.num_inputs &&
                               ctx->config->lds_size;

        const GnmPsShader psh = {
            .common =
                {
                    .shadersize =
                        codesize + sizeof(GnmShaderBinaryInfo),
                    .numinputusageslots = numinputslots,
                },
            .registers =
                {
                    .spishaderpgmlops = shspecificsize,
                    .spishaderpgmhips = 0xffffffff,
                    .spishaderpgmrsrc1ps = ctx->config->rsrc1,
                    .spishaderpgmrsrc2ps = ctx->config->rsrc2,
                    .spishaderzformat = ac_get_spi_shader_z_format(
                        ctx->rinfo->ps.writes_z,
                        ctx->rinfo->ps.writes_stencil,
                        ctx->rinfo->ps.writes_sample_mask,
                        ctx->rinfo->ps.writes_mrt0_alpha
                    ),
                    .spishadercolformat =
                        compact_spi_shader_col_format(ctx->rinfo->ps.spi_shader_col_format),
                    .spipsinputena = ctx->config->spi_ps_input_ena,
                    .spipsinputaddr = ctx->config->spi_ps_input_addr,
                    .spipsincontrol =
                        S_0286D8_NUM_INTERP(ctx->rinfo->ps.num_inputs) |
                        S_0286D8_PS_W32_EN(
                            ctx->rinfo->wave_size == 32
                        ) |
                        S_0286D8_PARAM_GEN(param_gen),
                    .spibaryccntl = S_0286E0_FRONT_FACE_ALL_BITS(0),
                    .dbshadercontrol = compute_db_shader_control(ctx->rinfo),
                    .cbshadermask = ac_get_cb_shader_mask(
                        ctx->rinfo->ps.spi_shader_col_format
                    ),
                },
            .numinputsemantics =
                util_bitcount64(ctx->nir->info.inputs_read),
        };
        memcpy(buf + offset, &psh, sizeof(psh));
        offset += sizeof(psh);
        break;
    }
    case PSBC_STAGE_COMPUTE: {
        const GnmCsShader csh = {
            .common =
                {
                    .shadersize =
                        codesize + sizeof(GnmShaderBinaryInfo),
                    .numinputusageslots = numinputslots,
                },
            .registers =
                {
                    .computepgmlo = shspecificsize,
                    .computepgmhi = 0,
                    .computepgmrsrc1 = ctx->config->rsrc1,
                    .computepgmrsrc2 = ctx->config->rsrc2,
                    /* Thread group size from NIR compute shader info */
                    .computenumthreadx =
                        ctx->nir->info.workgroup_size[0] ?
                        ctx->nir->info.workgroup_size[0] : 1,
                    .computenumthready =
                        ctx->nir->info.workgroup_size[1] ?
                        ctx->nir->info.workgroup_size[1] : 1,
                    .computenumthreadz =
                        ctx->nir->info.workgroup_size[2] ?
                        ctx->nir->info.workgroup_size[2] : 1,
                },
        };
        memcpy(buf + offset, &csh, sizeof(csh));
        offset += sizeof(csh);
        break;
    }
    case PSBC_STAGE_GEOMETRY: {
        /* Map mesa_prim to VGT_GS_OUT_PRIM_TYPE values */
        uint32_t gs_out_prim;
        switch (ctx->nir->info.gs.output_primitive) {
        case MESA_PRIM_POINTS:       gs_out_prim = V_028A6C_POINTLIST; break;
        case MESA_PRIM_LINE_STRIP:     gs_out_prim = V_028A6C_LINESTRIP; break;
        case MESA_PRIM_TRIANGLE_STRIP: gs_out_prim = V_028A6C_TRISTRIP;  break;
        default:                     gs_out_prim = V_028A6C_TRISTRIP;  break;
        }

        const GnmGsShader gsh = {
            .common =
                {
                    .shadersize =
                        codesize + sizeof(GnmShaderBinaryInfo),
                    .numinputusageslots = numinputslots,
                },
            .registers =
                {
                    .spishaderpgmlogs = shspecificsize,
                    .spishaderpgmhigs = 0xffffffff,
                    .spishaderpgmrsrc1gs = ctx->config->rsrc1,
                    .spishaderpgmrsrc2gs = ctx->config->rsrc2,
                    .vgtstrmoutconfig = 0,
                    .vgtgsoutprimtype = gs_out_prim,
                    .vgtgsinstancecnt =
                        S_028B90_CNT(MIN2(ctx->rinfo->gs.invocations, 127)) |
                        S_028B90_ENABLE(ctx->rinfo->gs.invocations > 0),
                },
            .numinputsemantics =
                util_bitcount64(ctx->nir->info.inputs_read),
            .numexportsemantics = outputswritten,
        };
        memcpy(buf + offset, &gsh, sizeof(gsh));
        offset += sizeof(gsh);
        break;
    }
    case PSBC_STAGE_TESS_CTRL: {
        /* Map tess primitive mode to VGT_TF_PARAM TYPE field */
        uint32_t tf_type;
        switch (ctx->nir->info.tess._primitive_mode) {
        case TESS_PRIMITIVE_ISOLINES:   tf_type = V_028B6C_TESS_ISOLINE;  break;
        case TESS_PRIMITIVE_TRIANGLES:  tf_type = V_028B6C_TESS_TRIANGLE; break;
        case TESS_PRIMITIVE_QUADS:      tf_type = V_028B6C_TESS_QUAD;     break;
        default:                        tf_type = V_028B6C_TESS_TRIANGLE; break;
        }

        /* Map tess spacing to VGT_TF_PARAM PARTITIONING field */
        uint32_t tf_partition;
        switch (ctx->nir->info.tess.spacing) {
        case TESS_SPACING_EQUAL:           tf_partition = V_028B6C_PART_INTEGER;    break;
        case TESS_SPACING_FRACTIONAL_ODD:  tf_partition = V_028B6C_PART_FRAC_ODD;   break;
        case TESS_SPACING_FRACTIONAL_EVEN: tf_partition = V_028B6C_PART_FRAC_EVEN;  break;
        default:                           tf_partition = V_028B6C_PART_INTEGER;    break;
        }

        /* Map CCW + point_mode to VGT_TF_PARAM TOPOLOGY field */
        uint32_t tf_topology;
        if (ctx->nir->info.tess.point_mode) {
            tf_topology = V_028B6C_OUTPUT_POINT;
        } else if (ctx->nir->info.tess.ccw) {
            tf_topology = V_028B6C_OUTPUT_TRIANGLE_CCW;
        } else {
            tf_topology = V_028B6C_OUTPUT_TRIANGLE_CW;
        }

        const uint32_t tf_param =
            S_028B6C_TYPE(tf_type) |
            S_028B6C_PARTITIONING(tf_partition) |
            S_028B6C_TOPOLOGY(tf_topology);

        const GnmHsShader hsh = {
            .common =
                {
                    .shadersize =
                        codesize + sizeof(GnmShaderBinaryInfo),
                    .numinputusageslots = numinputslots,
                },
            .registers =
                {
                    .spishaderpgmlohs = shspecificsize,
                    .spishaderpgmhihs = 0xffffffff,
                    .spishaderpgmrsrc1hs = ctx->config->rsrc1,
                    .spishaderpgmrsrc2hs = ctx->config->rsrc2,
                    .vgttfparam = tf_param,
                    /* Tessellation levels — use defaults if not available */
                    .vgthosmaxtesslevel = 64,
                    .vgthosmintesslevel = 1,
                },
            .numinputsemantics =
                util_bitcount64(ctx->nir->info.inputs_read),
        };
        memcpy(buf + offset, &hsh, sizeof(hsh));
        offset += sizeof(hsh);
        break;
    }
    case PSBC_STAGE_EXPORT: {
        /* ES (Export Shader) — VS variant with ES registers.
         * Runs before GS in a geometry pipeline. */
        const GnmEsShader esh = {
            .common =
                {
                    .shadersize =
                        codesize + sizeof(GnmShaderBinaryInfo),
                    .numinputusageslots = numinputslots,
                },
            .registers =
                {
                    .spishaderpgmloes = shspecificsize,
                    .spishaderpgmhies = 0xffffffff,
                    .spishaderpgmrsrc1es = ctx->config->rsrc1,
                    .spishaderpgmrsrc2es = ctx->config->rsrc2,
                },
            .numinputsemantics =
                util_bitcount64(ctx->nir->info.inputs_read),
            .numexportsemantics = outputswritten,
        };
        memcpy(buf + offset, &esh, sizeof(esh));
        offset += sizeof(esh);
        break;
    }
    case PSBC_STAGE_LOCAL: {
        /* LS (Local Shader) — VS variant with LS registers.
         * Runs before HS in a tessellation pipeline. */
        const GnmLsShader lsh = {
            .common =
                {
                    .shadersize =
                        codesize + sizeof(GnmShaderBinaryInfo),
                    .numinputusageslots = numinputslots,
                },
            .registers =
                {
                    .spishaderpgmlols = shspecificsize,
                    .spishaderpgmhils = 0xffffffff,
                    .spishaderpgmrsrc1ls = ctx->config->rsrc1,
                    .spishaderpgmrsrc2ls = ctx->config->rsrc2,
                },
            .numinputsemantics =
                util_bitcount64(ctx->nir->info.inputs_read),
            .numexportsemantics = outputswritten,
        };
        memcpy(buf + offset, &lsh, sizeof(lsh));
        offset += sizeof(lsh);
        break;
    }
    default:
        free(newcode);
        free(buf);
        return PSBC_RESULT_UNSUPPORTED_STAGE;
    }

    /* write shader common data: input usage slots */
    if (is_vs_family && ctx->rargs->prolog_inputs.used) {
        const GnmInputUsageSlot s = {
            .usagetype = GNM_SHINPUTUSAGE_SUBPTR_FETCHSHADER,
            .startregister =
                ctx->rargs->ac.args[ctx->rargs->prolog_inputs.arg_index]
                    .offset,
        };
        memcpy(buf + offset, &s, sizeof(s));
        offset += sizeof(s);
    }
    if (is_vs_family && ctx->rargs->ac.vertex_buffers.used) {
        const GnmInputUsageSlot s = {
            .usagetype = GNM_SHINPUTUSAGE_PTR_VERTEXBUFFERTABLE,
            .startregister =
                ctx->rargs->ac
                    .args[ctx->rargs->ac.vertex_buffers.arg_index]
                    .offset,
        };
        memcpy(buf + offset, &s, sizeof(s));
        offset += sizeof(s);
    }
    if (ctx->rargs->descriptors[0].used) {
        const GnmInputUsageSlot s = {
            .usagetype = GNM_SHINPUTUSAGE_PTR_INDIRECTRESOURCETABLE,
            .startregister =
                ctx->rargs->ac
                    .args[ctx->rargs->descriptors[0].arg_index]
                    .offset,
        };
        memcpy(buf + offset, &s, sizeof(s));
        offset += sizeof(s);
    }

    /* write input/export semantics */
    switch (ctx->stage) {
    case MESA_SHADER_VERTEX:
    case MESA_SHADER_TESS_EVAL:  /* DS uses same semantics as VS */
        for (uint32_t i = 0;
             i < util_bitcount64(ctx->nir->info.inputs_read); i += 1) {
            const GnmVertexInputSemantic input = {
                .semantic = i,
                .vgpr =
                    ctx->rargs->ac
                        .args[ctx->rargs->vs_inputs[i].arg_index]
                        .offset,
                .sizeinelements =
                    ctx->rargs->ac
                        .args[ctx->rargs->vs_inputs[i].arg_index]
                        .size,
            };
            memcpy(buf + offset, &input, sizeof(input));
            offset += sizeof(input);
        }
        for (uint32_t i = 0; i < outputswritten; i += 1) {
            const GnmVertexExportSemantic out = {
                .semantic = 15 + i,
                .outindex = i,
                .exportf16 = 0,
            };
            memcpy(buf + offset, &out, sizeof(out));
            offset += sizeof(out);
        }
        break;
    case MESA_SHADER_FRAGMENT:
        for (uint32_t i = 0; i < ctx->input_semantics->count; i += 1) {
            const uint32_t word = ctx->input_semantics->words[i];
            const GnmPixelInputSemantic input = {
                .semantic = word & 0xffu,
                .isflatshaded = (word >> 22) & 1u,
            };
            memcpy(buf + offset, &input, sizeof(input));
            offset += sizeof(input);
        }
        break;
    case MESA_SHADER_GEOMETRY:
        /* GS input/export semantics use the same vertex semantic format */
        for (uint32_t i = 0;
             i < util_bitcount64(ctx->nir->info.inputs_read); i += 1) {
            const GnmVertexInputSemantic input = {
                .semantic = i,
            };
            memcpy(buf + offset, &input, sizeof(input));
            offset += sizeof(input);
        }
        for (uint32_t i = 0; i < outputswritten; i += 1) {
            const GnmVertexExportSemantic out = {
                .semantic = 15 + i,
                .outindex = i,
                .exportf16 = 0,
            };
            memcpy(buf + offset, &out, sizeof(out));
            offset += sizeof(out);
        }
        break;
    case MESA_SHADER_TESS_CTRL:
        /* HS input semantics describe which VS outputs are read.
         * No export semantics — HS outputs go to LDS for DS. */
        for (uint32_t i = 0;
             i < util_bitcount64(ctx->nir->info.inputs_read); i += 1) {
            const GnmVertexInputSemantic input = {
                .semantic = i,
            };
            memcpy(buf + offset, &input, sizeof(input));
            offset += sizeof(input);
        }
        break;
    default:
        break;
    }

    /* alignment padding */
    memset(buf + offset, 0, alignbytes);
    offset += alignbytes;

    /* shader code */
    memcpy(buf + offset, newcode, codesize);
    offset += codesize;

    /* shader binary info */
    /* Compute chunkusagebaseoffsetdwords: offset from OrbShdr back to the
     * input usage slot table, in dwords.
     * Input usage slots are at: PSSL_HEADER_SIZE + GNM_FILE_HEADER_SIZE + headershsize
     * OrbShdr is at: current offset (= PSSL_HEADER_SIZE + GNM_FILE_HEADER_SIZE + shspecificsize + codesize)
     * Distance = offset - (PSSL_HEADER_SIZE + GNM_FILE_HEADER_SIZE + headershsize)
     * But the comment says "starts at ((uint32_t*)&ShaderBinaryInfo) - chunkusagebaseoffsetdwords"
     * so chunkusagebaseoffsetdwords = distance / 4
     * Note: if numinputslots == 0, the offset is 0 (no table to point to). */
    const size_t inputslots_offset =
        sizeof(PsslBinaryHeader) + sizeof(GnmShaderFileHeader) +
        headershsize(ctx->psbc_stage);
    const size_t orbshdr_offset = offset;
    const uint32_t chunkusageoffset =
        numinputslots > 0
            ? (uint32_t)((orbshdr_offset - inputslots_offset) / 4)
            : 0;

    /* Compute shader hash from SPIR-V data.
     * The PS4 uses this for shader cache identification.
     * We use a simple hash of the SPIR-V binary. */
    uint64_t shaderhash = 0;
    if (ctx->spirv_data && ctx->spirv_size > 0) {
        /* FNV-1a hash of the SPIR-V bytes */
        const uint8_t* sp = (const uint8_t*)ctx->spirv_data;
        shaderhash = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < ctx->spirv_size; i += 1) {
            shaderhash ^= sp[i];
            shaderhash *= 0x100000001b3ULL;
        }
    }

    GnmShaderBinaryInfo bininfo = {
        .signature = GNM_SHADER_BINARY_INFO_MAGIC,
        .version = 7,
        .ispsslcg = 1,
        .type = shbintype(ctx->psbc_stage),
        .length = codesize,
        .chunkusagebaseoffsetdwords = chunkusageoffset,
        .numinputusageslots = numinputslots,
        .shaderhash0 = (uint32_t)(shaderhash & 0xffffffff),
        .shaderhash1 = (uint32_t)(shaderhash >> 32),
    };
    bininfo.crc32 = hashsb(newcode, codesize, &bininfo);
    memcpy(buf + offset, &bininfo, sizeof(bininfo));
    offset += sizeof(bininfo);

    /* param info */
    const PsslBinaryParamInfo paraminfo = {0};
    memcpy(buf + offset, &paraminfo, sizeof(paraminfo));
    offset += sizeof(paraminfo);

    free(newcode);

    *out_data = buf;
    *out_size = offset;
    return PSBC_RESULT_OK;
}

/* === GPU target setup === */

static void setup_ac_info(struct ac_compiler_info* ac_info, enum amd_gfx_level gfxlevel) {
    ac_info->gfx_level = gfxlevel;
    ac_info->lds_size_per_workgroup = gfxlevel >= GFX7 ? 64 * 1024 : 32 * 1024;

    if (gfxlevel >= GFX10_3) {
        ac_info->max_waves_per_simd = 16;
        ac_info->num_physical_sgprs_per_simd = 108 * 16;
        ac_info->num_physical_wave64_vgprs_per_simd = 512;
        ac_info->num_simd_per_compute_unit = 2;
        ac_info->min_sgpr_alloc = 108;
        ac_info->max_sgpr_alloc = 108;
        ac_info->sgpr_alloc_granularity = 108;
        ac_info->min_wave64_vgpr_alloc = 8;
        ac_info->max_vgpr_alloc = 256;
        ac_info->wave64_vgpr_alloc_granularity = 8;
        ac_info->has_packed_math_16bit = true;
        /* PS5/GFX1013 does not implement accelerated integer dot products. */
        ac_info->has_fma_mix = true;
    } else if (gfxlevel == GFX10) {
        ac_info->max_waves_per_simd = 20;
        ac_info->num_physical_sgprs_per_simd = 128 * 20;
        ac_info->num_physical_wave64_vgprs_per_simd = 256;
        ac_info->num_simd_per_compute_unit = 2;
        ac_info->min_sgpr_alloc = 1;
        ac_info->max_sgpr_alloc = 128;
        ac_info->sgpr_alloc_granularity = 1;
        ac_info->min_wave64_vgpr_alloc = 4;
        ac_info->max_vgpr_alloc = 256;
        ac_info->wave64_vgpr_alloc_granularity = 4;
        ac_info->has_packed_math_16bit = true;
    } else if (gfxlevel == GFX8) {
        ac_info->max_waves_per_simd = 10;
        ac_info->num_physical_sgprs_per_simd = 16 * 10;
        ac_info->num_physical_wave64_vgprs_per_simd = 128;
        ac_info->num_simd_per_compute_unit = 1;
        ac_info->min_sgpr_alloc = 1;
        ac_info->max_sgpr_alloc = 16;
        ac_info->sgpr_alloc_granularity = 1;
        ac_info->min_wave64_vgpr_alloc = 4;
        ac_info->max_vgpr_alloc = 128;
        ac_info->wave64_vgpr_alloc_granularity = 4;
    } else {
        /* GFX7 */
        ac_info->max_waves_per_simd = 10;
        ac_info->num_physical_sgprs_per_simd = 12 * 10;
        ac_info->num_physical_wave64_vgprs_per_simd = 64;
        ac_info->num_simd_per_compute_unit = 1;
        ac_info->min_sgpr_alloc = 1;
        ac_info->max_sgpr_alloc = 12;
        ac_info->sgpr_alloc_granularity = 1;
        ac_info->min_wave64_vgpr_alloc = 4;
        ac_info->max_vgpr_alloc = 64;
        ac_info->wave64_vgpr_alloc_granularity = 4;
    }
}

static void setup_target(PsbcTarget target,
                         enum amd_gfx_level* gfxlevel,
                         enum radeon_family* chipfamily,
                         bool* neo) {
    switch (target) {
    case PSBC_TARGET_PS5:
        *gfxlevel = GFX10_3;
        *chipfamily = CHIP_NAVI21;
        *neo = false;
        break;
    case PSBC_TARGET_PS4_NEO:
        *gfxlevel = GFX8;
        *chipfamily = CHIP_TONGA;
        *neo = true;
        break;
    case PSBC_TARGET_PS4_BASE:
    default:
        *gfxlevel = GFX7;
        *chipfamily = CHIP_KAVERI;
        *neo = false;
        break;
    }
}

static pthread_once_t g_ps5_nir_options_once = PTHREAD_ONCE_INIT;
static struct ac_compiler_info g_ps5_nir_ac_info;
static struct radv_compiler_info g_ps5_nir_compiler_info;

static void init_ps5_nir_options(void) {
    setup_ac_info(&g_ps5_nir_ac_info, GFX10_3);
    g_ps5_nir_compiler_info.ac = &g_ps5_nir_ac_info;
    g_ps5_nir_compiler_info.key.family = CHIP_NAVI21;
    g_ps5_nir_compiler_info.key.ge_wave_size = 64;
    g_ps5_nir_compiler_info.key.ps_wave_size = 32;
    g_ps5_nir_compiler_info.key.cs_wave_size = 32;
    g_ps5_nir_compiler_info.key.rt_wave_size = 64;
    radv_get_nir_options(&g_ps5_nir_compiler_info);
}

const struct nir_shader_compiler_options*
psbc_get_nir_options(PsbcStage stage) {
    const mesa_shader_stage mesa_stage = psbc_to_mesa_stage(stage);

    if (mesa_stage == MESA_SHADER_NONE ||
        mesa_stage >= MESA_VULKAN_SHADER_STAGES)
        return NULL;
    pthread_once(&g_ps5_nir_options_once, init_ps5_nir_options);
    return &g_ps5_nir_compiler_info.nir_options[mesa_stage];
}

/* === Main compilation function === */

static bool psbc_licm_speculatable(nir_instr* instr, nir_loop* loop,
                                   bool instr_block_dominates_exit) {
    (void)loop;
    (void)instr_block_dominates_exit;
    return nir_instr_can_speculate(instr);
}

static bool compute_shape_valid(const nir_shader* nir) {
    if (nir->info.stage != MESA_SHADER_COMPUTE)
        return true;
    uint32_t invocations = 1;
    if (nir->info.workgroup_size_variable || nir->info.shared_size > 65536)
        return false;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned size = nir->info.workgroup_size[i];
        if (!size || size > 1024 / invocations)
            return false;
        invocations *= size;
    }
    return true;
}

/* Legacy Gallium indices are set-0 bindings, never bindless heap indices.
 * Check actual instructions rather than trusting caller-provided usage masks.
 * This boundary is shared by standalone stages and both halves of VS/GS. */
static bool legacy_texture_bindings_valid(const nir_shader* nir,
                                          const PsbcCompileOptions* opts) {
    nir_foreach_function_impl(impl, nir) {
        nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
                if (instr->type != nir_instr_type_tex)
                    continue;
                const nir_tex_instr* tex = nir_instr_as_tex(instr);
                for (unsigned sampler = 0; sampler < 2; ++sampler) {
                    if (sampler && (!nir_tex_instr_need_sampler(tex) || tex->embedded_sampler))
                        continue;
                    if (nir_tex_instr_src_index(tex, sampler ? nir_tex_src_sampler_deref : nir_tex_src_texture_deref) >= 0 ||
                        nir_tex_instr_src_index(tex, sampler ? nir_tex_src_sampler_handle : nir_tex_src_texture_handle) >= 0 ||
                        nir_tex_instr_src_index(tex, sampler ? nir_tex_src_sampler_heap_offset : nir_tex_src_texture_heap_offset) >= 0)
                        continue;
                    unsigned key = sampler ? tex->sampler_index : tex->texture_index;
                    bool found = false;
                    for (unsigned i = 0; i < opts->descriptor_binding_count; ++i) {
                        const PsbcDescriptorBinding* binding = &opts->descriptor_bindings[i];
                        found |= !binding->set && binding->binding == key &&
                                 binding->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER;
                    }
                    if (!found)
                        return false;
                }
            }
        }
    }
    return true;
}

static bool internal_nir_entrypoint_valid(const nir_shader* nir) {
    if (exec_list_length(&nir->functions) != 1)
        return false;
    nir_foreach_function(function, nir)
        return function->is_entrypoint && function->impl && !function->num_params;
    return false;
}

static bool psbc_descriptor_options_valid(const PsbcCompileOptions* opts) {
    if (opts->descriptor_binding_count > PSBC_MAX_DESCRIPTOR_BINDINGS)
        return false;
    for (uint32_t i = 0; i < opts->descriptor_binding_count; ++i) {
        const PsbcDescriptorBinding* binding = &opts->descriptor_bindings[i];
        const bool valid_type =
            binding->type == PSBC_DESCRIPTOR_UNIFORM_BUFFER ||
            binding->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ||
            binding->type == PSBC_DESCRIPTOR_STORAGE_BUFFER ||
            binding->type == PSBC_DESCRIPTOR_STORAGE_IMAGE;
        const uint32_t expected_stride =
            binding->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER ? 48u :
            binding->type == PSBC_DESCRIPTOR_STORAGE_IMAGE ? 32u : 16u;
        if (binding->set != 0 || binding->binding >= PSBC_MAX_DESCRIPTOR_BINDINGS ||
            !valid_type || !binding->array_size ||
            binding->stride != expected_stride ||
            (binding->offset & 15u) ||
            (uint64_t)binding->offset +
                    (uint64_t)binding->array_size * binding->stride >
                UINT32_MAX)
            return false;
        for (uint32_t j = 0; j < i; ++j)
            if (opts->descriptor_bindings[j].set == binding->set &&
                opts->descriptor_bindings[j].binding == binding->binding)
                return false;
    }
    return true;
}

static void psbc_descriptor_layout(const PsbcCompileOptions* opts,
                                   void* descriptor_set0_storage,
                                   struct radv_shader_layout* layout) {
    if (opts->descriptor_binding_count) {
        struct radv_descriptor_set_layout* set_layout =
            (struct radv_descriptor_set_layout*)descriptor_set0_storage;
        uint32_t binding_count = 0;
        uint32_t set_size = 0;
        for (uint32_t i = 0; i < opts->descriptor_binding_count; ++i) {
            const PsbcDescriptorBinding* source =
                &opts->descriptor_bindings[i];
            struct radv_descriptor_set_binding_layout* target =
                &set_layout->binding[source->binding];
            target->type = source->type == PSBC_DESCRIPTOR_UNIFORM_BUFFER
                               ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                           : source->type == PSBC_DESCRIPTOR_STORAGE_BUFFER
                               ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                           : source->type == PSBC_DESCRIPTOR_STORAGE_IMAGE
                               ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                               : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            target->array_size = source->type == PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER
                ? contiguous_sampler_count(opts, source) : source->array_size;
            target->offset = source->offset;
            target->size = source->stride;
            binding_count = MAX2(binding_count, source->binding + 1u);
            set_size = MAX2(set_size, source->offset +
                                      source->array_size * source->stride);
        }
        set_layout->binding_count = binding_count;
        set_layout->size = set_size;
        layout->num_sets = 1;
        layout->set[0].layout = set_layout;
    }
}


static bool
lower_gallium_tess_levels(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
    (void)data;
    if (b->shader->info.stage == MESA_SHADER_TESS_CTRL &&
        intr->intrinsic == nir_intrinsic_store_output) {
        nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
        if (sem.no_varying && (sem.location == VARYING_SLOT_TESS_LEVEL_INNER ||
                               sem.location == VARYING_SLOT_TESS_LEVEL_OUTER)) {
            /* Gallium's previous link saw TES system values, not patch inputs.
             * Re-linking below decides which factor copies remain necessary. */
            sem.no_varying = false;
            nir_intrinsic_set_io_semantics(intr, sem);
            return true;
        }
    }
    if (b->shader->info.stage != MESA_SHADER_TESS_EVAL ||
        (intr->intrinsic != nir_intrinsic_load_tess_level_inner &&
         intr->intrinsic != nir_intrinsic_load_tess_level_outer))
        return false;

    /* Gallium exposes these as system values; RADV links them as patch IO. */
    unsigned slot = intr->intrinsic == nir_intrinsic_load_tess_level_inner ?
        VARYING_SLOT_TESS_LEVEL_INNER : VARYING_SLOT_TESS_LEVEL_OUTER;
    b->cursor = nir_before_instr(&intr->instr);
    nir_def *value = nir_load_input(b, intr->def.num_components, 32,
        nir_imm_int(b, 0), .dest_type = nir_type_float32,
        .io_semantics = {.location = slot, .num_slots = 1});
    nir_def_replace(&intr->def, value);
    return true;
}

static nir_shader* prepare_stage_nir(
    const struct radv_compiler_info* compiler_info,
    struct radv_shader_stage* stage,
    const uint32_t* spirv,
    size_t spirv_size,
    const nir_shader* input_nir,
    const PsbcCompileOptions* opts
) {
    if (input_nir) {
        const bool entrypoint = internal_nir_entrypoint_valid(input_nir);
        const bool shape = compute_shape_valid(input_nir);
        const bool bindings = legacy_texture_bindings_valid(input_nir, opts);
        if (!entrypoint || !shape || !bindings) {
            fprintf(stderr, "PSBC prepare failure: entrypoint=%u shape=%u bindings=%u functions=%u\n",
                    entrypoint, shape, bindings,
                    exec_list_length(&input_nir->functions));
            return NULL;
        }
    }
    stage->key.ps5_compact_vertex_inputs = input_nir &&
        input_nir->info.stage == MESA_SHADER_VERTEX && input_nir->info.io_lowered;
    stage->spirv.data = (const char*)spirv;
    stage->spirv.size = spirv_size;
    stage->entrypoint = opts->entrypoint ? opts->entrypoint : "main";
    stage->key.optimisations_disabled = !opts->optimise;

    /* RADV updates internal_nir->options before making its own clone. Never
     * let that write escape into caller-owned NIR (or retain stack options). */
    if (input_nir) {
        stage->internal_nir = nir_shader_clone(NULL, input_nir);
        if (!stage->internal_nir)
            return NULL;
    }

    const struct radv_spirv_to_nir_options spirv_options = {
        .lower_view_index_to_zero = true,
        .lower_view_index_to_device_index = false,
    };
    nir_shader* nir = radv_shader_spirv_to_nir(
        compiler_info, stage, &spirv_options, false
    );
    if (input_nir) {
        ralloc_free(stage->internal_nir);
        stage->internal_nir = NULL;
    }
    if (!nir) {
        fprintf(stderr, "PSBC prepare failure: radv-import\n");
        return NULL;
    }
    if (!compute_shape_valid(nir)) {
        ralloc_free(nir);
        return NULL;
    }

    if (input_nir) {
        struct gallium_buffer_state buffers = {.options = opts, .valid = true};
        nir_shader_instructions_pass(nir, lower_gallium_buffer_index,
                                     nir_metadata_control_flow, &buffers);
        nir_shader_instructions_pass(nir, lower_gallium_texture_index,
                                     nir_metadata_control_flow, &buffers);
        nir_shader_instructions_pass(nir, lower_gallium_image_index,
                                     nir_metadata_control_flow, &buffers);
        if (!buffers.valid) {
            fprintf(stderr, "PSBC prepare failure: gallium-descriptors\n");
            ralloc_free(nir);
            return NULL;
        }

        /* radv_shader_spirv_to_nir() normalizes these only on its SPIR-V
         * path.  Gallium supplies internal NIR, while ACO accepts only the
         * normalized 2*pi operations. */
        NIR_PASS(_, nir, nir_normalize_sin_cos);
        NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_gallium_tess_levels,
                 nir_metadata_control_flow, NULL);
    }

    radv_optimize_nir(nir, !opts->optimise);

    bool indirect_derefs_lowered = false;
    NIR_PASS(indirect_derefs_lowered, nir, ac_nir_lower_indirect_derefs);
    NIR_PASS(_, nir, nir_lower_vars_to_ssa);
    if (indirect_derefs_lowered) {
        NIR_PASS(_, nir, nir_lower_undef_to_zero, NULL);
        const nir_opt_peephole_select_options flatten_indirect_arrays = {
            .limit = 10,
            .indirect_load_ok = true,
            .expensive_alu_ok = true,
            .discard_ok = true,
        };
        NIR_PASS(_, nir, nir_opt_peephole_select,
                 &flatten_indirect_arrays);
        if (opts->optimise)
            radv_optimize_nir(nir, false);
    }
    nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
    radv_nir_lower_io(nir);
    if (input_nir && stage->next_stage == MESA_SHADER_FRAGMENT &&
        (nir->info.outputs_written & VARYING_BIT_CLIP_VERTEX)) {
        /* Gallium's state tracker lowers enabled user clip planes into
         * CLIP_DIST before this compiler receives the shader. With no active
         * planes, compatibility GLSL can retain gl_ClipVertex even though it
         * is neither a rasterization output nor a fragment varying. Remove
         * only that final-stage output; keep generated clip distances and
         * intermediate VS-to-GS outputs intact. The NIR helper retains XFB. */
        NIR_PASS(_, nir, nir_remove_outputs, MESA_SHADER_FRAGMENT,
                 VARYING_BIT_CLIP_VERTEX, VARYING_BIT_CLIP_VERTEX);
        NIR_PASS(_, nir, nir_opt_dce);
        nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
    }
    stage->nir = nir;
    return nir;
}


/* Fixed compiler-only tessellation graph. Link ordering and mode merging follow
 * RADV radv_pipeline_graphics.c; the extracted host proof is the reference.
 * ponytail: position + one scalar patch varying only; widen with linked tests. */
static const mesa_shader_stage psbc_tess_order[] = {
    MESA_SHADER_VERTEX, MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL,
    MESA_SHADER_GEOMETRY
};

static enum pipe_format psbc_vertex_pipe_format(PsbcVertexFormat format)
{
    switch (format) {
    case PSBC_VERTEX_FORMAT_R32_FLOAT: return PIPE_FORMAT_R32_FLOAT;
    case PSBC_VERTEX_FORMAT_R32G32_FLOAT: return PIPE_FORMAT_R32G32_FLOAT;
    case PSBC_VERTEX_FORMAT_R32G32B32_FLOAT: return PIPE_FORMAT_R32G32B32_FLOAT;
    case PSBC_VERTEX_FORMAT_R32G32B32A32_FLOAT: return PIPE_FORMAT_R32G32B32A32_FLOAT;
    case PSBC_VERTEX_FORMAT_R64_FLOAT: return PIPE_FORMAT_R64_FLOAT;
    case PSBC_VERTEX_FORMAT_R64G64_FLOAT: return PIPE_FORMAT_R64G64_FLOAT;
    case PSBC_VERTEX_FORMAT_R64G64B64_FLOAT: return PIPE_FORMAT_R64G64B64_FLOAT;
    case PSBC_VERTEX_FORMAT_R64G64B64A64_FLOAT: return PIPE_FORMAT_R64G64B64A64_FLOAT;
    case PSBC_VERTEX_FORMAT_R11G11B10_FLOAT: return PIPE_FORMAT_R11G11B10_FLOAT;
    case PSBC_VERTEX_FORMAT_B8G8R8A8_UNORM: return PIPE_FORMAT_B8G8R8A8_UNORM;
    case PSBC_VERTEX_FORMAT_R8G8B8A8_UNORM: return PIPE_FORMAT_R8G8B8A8_UNORM;
    case PSBC_VERTEX_FORMAT_R10G10B10A2_UNORM: return PIPE_FORMAT_R10G10B10A2_UNORM;
    case PSBC_VERTEX_FORMAT_B10G10R10A2_UNORM: return PIPE_FORMAT_B10G10R10A2_UNORM;
    case PSBC_VERTEX_FORMAT_R10G10B10A2_SNORM: return PIPE_FORMAT_R10G10B10A2_SNORM;
    case PSBC_VERTEX_FORMAT_B10G10R10A2_SNORM: return PIPE_FORMAT_B10G10R10A2_SNORM;
    case PSBC_VERTEX_FORMAT_R10G10B10A2_USCALED: return PIPE_FORMAT_R10G10B10A2_USCALED;
    case PSBC_VERTEX_FORMAT_B10G10R10A2_USCALED: return PIPE_FORMAT_B10G10R10A2_USCALED;
    case PSBC_VERTEX_FORMAT_R10G10B10A2_SSCALED: return PIPE_FORMAT_R10G10B10A2_SSCALED;
    case PSBC_VERTEX_FORMAT_B10G10R10A2_SSCALED: return PIPE_FORMAT_B10G10R10A2_SSCALED;
    case PSBC_VERTEX_FORMAT_R32_SINT: return PIPE_FORMAT_R32_SINT;
    case PSBC_VERTEX_FORMAT_R32G32_SINT: return PIPE_FORMAT_R32G32_SINT;
    case PSBC_VERTEX_FORMAT_R32G32B32_SINT: return PIPE_FORMAT_R32G32B32_SINT;
    case PSBC_VERTEX_FORMAT_R32G32B32A32_SINT: return PIPE_FORMAT_R32G32B32A32_SINT;
    case PSBC_VERTEX_FORMAT_R32_UINT: return PIPE_FORMAT_R32_UINT;
    case PSBC_VERTEX_FORMAT_R32G32_UINT: return PIPE_FORMAT_R32G32_UINT;
    case PSBC_VERTEX_FORMAT_R32G32B32_UINT: return PIPE_FORMAT_R32G32B32_UINT;
    case PSBC_VERTEX_FORMAT_R32G32B32A32_UINT: return PIPE_FORMAT_R32G32B32A32_UINT;
    default: return PIPE_FORMAT_NONE;
    }
}

static bool psbc_apply_vertex_input_state(const PsbcCompileOptions *opts,
                                          struct radv_graphics_state_key *gfx)
{
    if (opts->vertex_attribute_count > PSBC_MAX_VERTEX_ATTRIBUTES)
        return false;
    for (uint32_t i = 0; i < opts->vertex_attribute_count; ++i) {
        const PsbcVertexAttribute *attribute = &opts->vertex_attributes[i];
        enum pipe_format format = psbc_vertex_pipe_format(attribute->format);
        if (format == PIPE_FORMAT_NONE ||
            attribute->location >= PSBC_MAX_VERTEX_ATTRIBUTES ||
            attribute->binding >= MAX_VBS ||
            (!attribute->stride && opts->target != PSBC_TARGET_PS5) ||
            !attribute->alignment)
            return false;
        const uint32_t location = attribute->location;
        gfx->vi.attributes_valid |= BITFIELD_BIT(location);
        gfx->vi.vertex_attribute_formats[location] = format;
        gfx->vi.vertex_attribute_bindings[location] = attribute->binding;
        gfx->vi.vertex_attribute_offsets[location] = attribute->offset;
        gfx->vi.vertex_attribute_strides[location] = attribute->stride;
        gfx->vi.vertex_binding_align[attribute->binding] = attribute->alignment;
        if (attribute->instance_divisor) {
            gfx->vi.instance_rate_inputs |= BITFIELD_BIT(location);
            gfx->vi.instance_rate_divisors[location] = attribute->instance_divisor;
        }
    }
    return true;
}

static bool psbc_tess_vertex_inputs_match(const nir_shader *vs,
                                          const PsbcCompileOptions *opts)
{
    uint64_t remaining = vs->info.inputs_read >> VERT_ATTRIB_GENERIC0;
    for (uint32_t i = 0; i < opts->vertex_attribute_count; ++i)
        remaining &= ~BITFIELD64_BIT(opts->vertex_attributes[i].location);
    if (remaining)
        return false;
    nir_foreach_shader_in_variable(var, vs) {
        if (var->data.location < VERT_ATTRIB_GENERIC0)
            continue;
        unsigned location = var->data.location - VERT_ATTRIB_GENERIC0;
        bool found = false;
        for (uint32_t i = 0; i < opts->vertex_attribute_count; ++i)
            found |= opts->vertex_attributes[i].location == location;
        if (!found)
            return false;
    }
    return true;
}

static bool psbc_tess_input_supported(const nir_shader* nir, bool buffers, bool streamout) {
    if (!internal_nir_entrypoint_valid(nir) ||
        (!buffers && (nir->info.num_ubos || nir->info.num_ssbos)) || nir->info.num_abos ||
        (!buffers && nir->info.num_images) || (!buffers && nir->info.num_textures) || nir->info.shared_size ||
        (!streamout && (nir->xfb_info || nir->info.has_transform_feedback_varyings)))
        return false;
    nir_foreach_variable_in_shader(v, nir) {
        /* GLSL can retain an unused gl_PatchVerticesIn constant declaration
         * after folding it. Live constant-memory derefs remain rejected below. */
        if (v->data.mode == nir_var_shader_temp || v->data.mode == nir_var_mem_constant)
            continue;
        if (buffers && (v->data.mode == nir_var_mem_ubo || v->data.mode == nir_var_mem_ssbo))
            continue;
        if (buffers && v->data.mode == nir_var_uniform &&
            glsl_type_is_sampler(glsl_without_array(v->type)))
            continue;
        if (buffers && v->data.mode == nir_var_image)
            continue;
        if (v->data.mode != nir_var_shader_in && v->data.mode != nir_var_shader_out)
            return false;
        bool input = v->data.mode == nir_var_shader_in;
        mesa_shader_stage s = nir->info.stage;
        const struct glsl_type* type = v->type;
        if (v->data.location_frac || v->data.compact)
            return false;
        if (v->data.location == VARYING_SLOT_POS && !v->data.patch) {
            if (s == MESA_SHADER_TESS_CTRL ||
                ((s == MESA_SHADER_TESS_EVAL || s == MESA_SHADER_GEOMETRY) && input)) {
                if (!glsl_type_is_array(type) || !glsl_get_length(type) ||
                    glsl_get_length(type) > 32)
                    return false;
                type = glsl_get_array_element(type);
            }
            if (type != glsl_vec4_type())
                return false;
        } else if (v->data.patch &&
                   ((s == MESA_SHADER_TESS_CTRL && !input) ||
                    (s == MESA_SHADER_TESS_EVAL && input))) {
            if (v->data.location >= VARYING_SLOT_PATCH0 &&
                v->data.location < VARYING_SLOT_TESS_MAX) {
                unsigned slots = glsl_count_attribute_slots(type, false);
                type = glsl_without_array(type);
                if (!slots || slots > VARYING_SLOT_TESS_MAX - v->data.location ||
                    (!glsl_type_is_scalar(type) && !glsl_type_is_vector(type)) ||
                    glsl_get_bit_size(type) > 64)
                    return false;
            } else if (v->data.location == VARYING_SLOT_TESS_LEVEL_OUTER ||
                       v->data.location == VARYING_SLOT_TESS_LEVEL_INNER) {
                unsigned count = v->data.location == VARYING_SLOT_TESS_LEVEL_OUTER ? 4 : 2;
                if (!glsl_type_is_array(type) || glsl_get_length(type) != count ||
                    glsl_get_array_element(type) != glsl_float_type())
                    return false;
            } else {
                return false;
            }
        } else {
            type = glsl_without_array(type);
            if ((!glsl_type_is_scalar(type) && !glsl_type_is_vector(type)) ||
                glsl_get_bit_size(type) > 64)
                return false;
        }
    }
    /* Do not trust caller-provided resource masks. This first API accepts
     * deref IO or the equivalent narrow Gallium I/O intrinsics, not arbitrary
     * pre-lowered resource/native ABI instructions. */
    nir_foreach_function_impl(impl, nir) {
        nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
                if ((!buffers && instr->type == nir_instr_type_tex) || instr->type == nir_instr_type_call)
                    return false;
                if (instr->type == nir_instr_type_deref) {
                    nir_deref_instr* d = nir_instr_as_deref(instr);
                    nir_variable_mode allowed = nir_var_shader_in | nir_var_shader_out |
                                                nir_var_shader_temp | nir_var_function_temp;
                    if (buffers)
                        allowed |= nir_var_image;
                    if (d->modes & ~allowed)
                        return false;
                }
                if (instr->type != nir_instr_type_intrinsic)
                    continue;
                nir_intrinsic_instr* intr = nir_instr_as_intrinsic(instr);
                switch (intr->intrinsic) {
                case nir_intrinsic_load_deref:
                case nir_intrinsic_store_deref:
                case nir_intrinsic_copy_deref:
                    break;
                case nir_intrinsic_load_per_vertex_input:
                    if ((nir->info.stage != MESA_SHADER_TESS_CTRL &&
                         nir->info.stage != MESA_SHADER_TESS_EVAL &&
                         nir->info.stage != MESA_SHADER_GEOMETRY) ||
                        !intr->num_components || intr->num_components > 4 ||
                        nir_intrinsic_component(intr) + intr->num_components > 4)
                        return false;
                    break;
                case nir_intrinsic_store_per_vertex_output:
                    if (nir->info.stage != MESA_SHADER_TESS_CTRL ||
                        !intr->num_components || intr->num_components > 4 ||
                        nir_intrinsic_component(intr) + intr->num_components > 4)
                        return false;
                    break;
                case nir_intrinsic_load_input:
                    if (!intr->num_components || intr->num_components > 4 ||
                        nir_intrinsic_component(intr) + intr->num_components > 4)
                        return false;
                    break;
                case nir_intrinsic_store_output: {
                    unsigned location = nir_intrinsic_io_semantics(intr).location;
                    bool position = (location == VARYING_SLOT_POS ||
                                     location == VARYING_SLOT_CLIP_DIST0 ||
                                     location == VARYING_SLOT_CLIP_DIST1 ||
                                     ((location == VARYING_SLOT_PSIZ ||
                                       location == VARYING_SLOT_LAYER ||
                                       location == VARYING_SLOT_VIEWPORT) &&
                                      intr->num_components == 1 &&
                                      nir_intrinsic_component(intr) == 0)) &&
                                    (nir->info.stage == MESA_SHADER_VERTEX ||
                                     nir->info.stage == MESA_SHADER_TESS_EVAL ||
                                     nir->info.stage == MESA_SHADER_GEOMETRY) &&
                                    nir_intrinsic_io_semantics(intr).num_slots == 1 &&
                                    (!nir_intrinsic_has_range(intr) || nir_intrinsic_range(intr) <= 1) &&
                                    intr->num_components && intr->num_components <= 4 &&
                                    nir_intrinsic_component(intr) + intr->num_components <= 4;
                    bool patch = nir->info.stage == MESA_SHADER_TESS_CTRL &&
                                 ((location >= VARYING_SLOT_PATCH0 && location < VARYING_SLOT_TESS_MAX &&
                                   intr->num_components && intr->num_components <= 4 &&
                                   nir_intrinsic_io_semantics(intr).num_slots >= 1 &&
                                   nir_intrinsic_io_semantics(intr).num_slots <= VARYING_SLOT_TESS_MAX - location &&
                                   (!nir_intrinsic_has_range(intr) || nir_intrinsic_range(intr) <= VARYING_SLOT_TESS_MAX - location)) ||
                                  (location == VARYING_SLOT_TESS_LEVEL_OUTER &&
                                   intr->num_components && intr->num_components <= 4 &&
                                   nir_intrinsic_io_semantics(intr).num_slots >= 1 &&
                                   nir_intrinsic_io_semantics(intr).num_slots <= 4 &&
                                   (!nir_intrinsic_has_range(intr) || nir_intrinsic_range(intr) <= 4)) ||
                                  (location == VARYING_SLOT_TESS_LEVEL_INNER &&
                                   intr->num_components && intr->num_components <= 2 &&
                                   nir_intrinsic_io_semantics(intr).num_slots >= 1 &&
                                   nir_intrinsic_io_semantics(intr).num_slots <= 2 &&
                                   (!nir_intrinsic_has_range(intr) || nir_intrinsic_range(intr) <= 2)));
                    bool varying = location >= VARYING_SLOT_VAR0 && location < VARYING_SLOT_MAX &&
                                   nir->info.stage != MESA_SHADER_FRAGMENT;
                    if ((!position && !patch && !varying) ||
                        nir_intrinsic_component(intr) + intr->num_components > 4)
                        return false;
                    break;
                }
                case nir_intrinsic_load_vertex_id_zero_base:
                case nir_intrinsic_load_first_vertex:
                    if (nir->info.stage != MESA_SHADER_VERTEX) return false;
                    break;
                case nir_intrinsic_load_invocation_id:
                    if (nir->info.stage != MESA_SHADER_TESS_CTRL) return false;
                    break;
                case nir_intrinsic_load_tess_coord:
                    if (nir->info.stage != MESA_SHADER_TESS_EVAL) return false;
                    break;
                default:
                    break;
                }
            }
        }
    }
    return true;
}

static void psbc_tess_fill_linked_io_info(struct radv_shader_stage* producer,
                                         struct radv_shader_stage* consumer) {
    if (producer->stage == MESA_SHADER_VERTEX) {
        unsigned slots = util_bitcount64(consumer->nir->info.inputs_read);
        producer->info.vs.num_linked_outputs = slots;
        consumer->info.tcs.num_linked_inputs = slots;
    } else if (producer->stage == MESA_SHADER_TESS_CTRL) {
        uint64_t factors = VARYING_BIT_TESS_LEVEL_OUTER | VARYING_BIT_TESS_LEVEL_INNER;
        consumer->info.tes.num_linked_inputs =
            util_bitcount64(consumer->nir->info.inputs_read & ~factors);
        consumer->info.tes.num_linked_patch_inputs =
            util_bitcount64(consumer->nir->info.inputs_read & factors) +
            util_bitcount(consumer->nir->info.patch_inputs_read);
    } else if (producer->stage == MESA_SHADER_TESS_EVAL) {
        unsigned slots = util_bitcount64(consumer->nir->info.inputs_read);
        producer->info.tes.num_linked_outputs = slots;
        consumer->info.gs.num_linked_inputs = slots;
    }
    producer->info.outputs_linked = true;
    consumer->info.inputs_linked = true;
}

/* Same allocation equation as ac_shader_util.c:get_tcs_wg_output_mem_size.
 * Check a single patch BEFORE RADV's patch-count calculation (which floors to
 * one even if capacity cannot hold it). Counts come from actual linked NIR. */
static unsigned psbc_tess_output_bytes(const ac_nir_tess_io_info* io,
                                       unsigned output_vertices,
                                       unsigned patches) {
    return align(16 * output_vertices * patches, AMD_MEMCHANNEL_INTERLEAVE_BYTES) *
               io->highest_remapped_vram_output +
           align(16 * patches, AMD_MEMCHANNEL_INTERLEAVE_BYTES) *
               io->highest_remapped_vram_patch_output;
}
static void
psbc_merge_tess_info(struct shader_info *tes_info, struct shader_info *tcs_info)
{
   /* The Vulkan 1.0.38 spec, section 21.1 Tessellator says:
    *
    *    "PointMode. Controls generation of points rather than triangles
    *     or lines. This functionality defaults to disabled, and is
    *     enabled if either shader stage includes the execution mode.
    *
    * and about Triangles, Quads, IsoLines, VertexOrderCw, VertexOrderCcw,
    * PointMode, SpacingEqual, SpacingFractionalEven, SpacingFractionalOdd,
    * and OutputVertices, it says:
    *
    *    "One mode must be set in at least one of the tessellation
    *     shader stages."
    *
    * So, the fields can be set in either the TCS or TES, but they must
    * agree if set in both.  Our backend looks at TES, so bitwise-or in
    * the values from the TCS.
    */
   assert(tcs_info->tess.tcs_vertices_out == 0 || tes_info->tess.tcs_vertices_out == 0 ||
          tcs_info->tess.tcs_vertices_out == tes_info->tess.tcs_vertices_out);
   tes_info->tess.tcs_vertices_out |= tcs_info->tess.tcs_vertices_out;

   assert(tcs_info->tess.spacing == TESS_SPACING_UNSPECIFIED || tes_info->tess.spacing == TESS_SPACING_UNSPECIFIED ||
          tcs_info->tess.spacing == tes_info->tess.spacing);
   tes_info->tess.spacing |= tcs_info->tess.spacing;

   assert(tcs_info->tess._primitive_mode == TESS_PRIMITIVE_UNSPECIFIED ||
          tes_info->tess._primitive_mode == TESS_PRIMITIVE_UNSPECIFIED ||
          tcs_info->tess._primitive_mode == tes_info->tess._primitive_mode);
   tes_info->tess._primitive_mode |= tcs_info->tess._primitive_mode;
   tes_info->tess.ccw |= tcs_info->tess.ccw;
   tes_info->tess.point_mode |= tcs_info->tess.point_mode;

   /* Copy the merged info back to the TCS */
   tcs_info->tess.tcs_vertices_out = tes_info->tess.tcs_vertices_out;
   tcs_info->tess._primitive_mode = tes_info->tess._primitive_mode;
}
static void
psbc_tess_link_varyings(struct radv_shader_stage *stages, enum amd_gfx_level gfx_level)
{
   /* Prepare shaders before running nir_opt_varyings. */
   for (int i = 0; i < ARRAY_SIZE(psbc_tess_order); ++i) {
      const mesa_shader_stage s = psbc_tess_order[i];
      if (!stages[s].nir)
         continue;

      if (stages[s].key.optimisations_disabled)
         continue;

      nir_shader *shader = stages[s].nir;

      /* It is expected by nir_opt_varyings that no undefined stores are present in the shader. */
      NIR_PASS(_, shader, nir_opt_undef);

      /* Update load/store alignments because inter-stage code motion may move instructions used to deduce this info. */
      NIR_PASS(_, shader, nir_opt_load_store_update_alignments);
   }

   int highest_changed_producer = -1;

   /* Optimize varyings from first to last stage. */
   for (int i = 0; i < ARRAY_SIZE(psbc_tess_order); ++i) {
      const mesa_shader_stage s = psbc_tess_order[i];
      const mesa_shader_stage next = stages[s].info.next_stage;
      if (!stages[s].nir || next == MESA_SHADER_NONE || !stages[next].nir)
         continue;

      if (stages[s].key.optimisations_disabled || stages[next].key.optimisations_disabled)
         continue;

      nir_shader *producer = stages[s].nir;
      nir_shader *consumer = stages[next].nir;

      const nir_opt_varyings_progress p = nir_opt_varyings(producer, consumer, true, 0, 0, false);

      /* Run algebraic optimizations on shaders that changed. */
      if (p & nir_progress_producer) {
         radv_optimize_nir_algebraic(producer, false, false, gfx_level);
         NIR_PASS(_, producer, nir_opt_undef);

         highest_changed_producer = i;
      }
      if (p & nir_progress_consumer) {
         radv_optimize_nir_algebraic(consumer, false, false, gfx_level);
         NIR_PASS(_, consumer, nir_opt_undef);
      }
   }

   /* Optimize varyings from last to first stage. */
   for (int i = highest_changed_producer; i >= 0; --i) {
      const mesa_shader_stage s = psbc_tess_order[i];
      const mesa_shader_stage next = stages[s].info.next_stage;
      if (!stages[s].nir || next == MESA_SHADER_NONE || !stages[next].nir)
         continue;

      if (stages[s].key.optimisations_disabled || stages[next].key.optimisations_disabled)
         continue;

      nir_shader *producer = stages[s].nir;
      nir_shader *consumer = stages[next].nir;

      const nir_opt_varyings_progress p = nir_opt_varyings(producer, consumer, true, 0, 0, false);

      /* Run algebraic optimizations on shaders that changed. */
      if (p & nir_progress_producer) {
         radv_optimize_nir_algebraic(producer, true, false, gfx_level);
         NIR_PASS(_, producer, nir_opt_undef);
      }
      if (p & nir_progress_consumer) {
         radv_optimize_nir_algebraic(consumer, true, false, gfx_level);
         NIR_PASS(_, consumer, nir_opt_undef);
      }
   }

   /* Run optimizations and fixups after linking. */
   for (int i = 0; i < ARRAY_SIZE(psbc_tess_order); ++i) {
      const mesa_shader_stage s = psbc_tess_order[i];
      if (!stages[s].nir)
         continue;

      nir_shader *shader = stages[s].nir;

      /* Re-vectorize I/O for stages that use memory for I/O (LDS or VRAM).
       * Don't vectorize FS I/O, doing so just regresses shader stats without any benefit.
       */
      if (s != MESA_SHADER_FRAGMENT && !stages[s].key.optimisations_disabled) {
         /* Delete dead instructions to prevent them from being vectorized. */
         NIR_PASS(_, shader, nir_opt_dce);

         /* Vectorize inputs. Non-FS inputs are always read from memory. */
         nir_variable_mode vec_mode = nir_var_shader_in;

         /* There is also no benefit from re-vectorizing the outputs of the last pre-rasterization
          * stage here, because ac_nir_lower_ngg/legacy already takes care of that.
          */
         if (!radv_is_last_vgt_stage(&stages[s]))
            vec_mode |= nir_var_shader_out;

         /* Scalarize and revectorize VS inputs to make sure every VS input is loaded by a
          * single *_load_format_* instruction. Those instructions can't skip loading unused
          * components before the last used component, so loading X, Y, Z, W separately
          * actually loads X, XY, XYZ, XYZW, which unnecessarily increases VMEM return data
          * transfers between the VMEM cache and the SIMDs, which wastes SIMD<->VMEM cache bandwidth.
          * By allowing holes during VS input vectorization, VS input loads loading different
          * components are always merged, so that no used or unused component is ever loaded twice.
          */
         if (s == MESA_SHADER_VERTEX) {
            NIR_PASS(_, shader, nir_opt_vectorize_io, nir_var_shader_in, true);
            vec_mode &= ~nir_var_shader_in;
         }

         if (vec_mode)
            NIR_PASS(_, shader, nir_opt_vectorize_io, vec_mode, false);
      }

      /* Gather shader info; at least the I/O info likely changed
       * and changes to only the I/O info are not reflected in nir_opt_varyings_progress.
       */
      nir_shader_gather_info(shader, nir_shader_get_entrypoint(shader));

      /* Recreate XFB info from intrinsics (nir_opt_varyings may have changed it). */
      if (shader->xfb_info) {
         nir_gather_xfb_info_from_intrinsics(shader);
      }
   }

   /* Fill linked I/O info.
    * This needs to be done after all optimizations are done and shader info gathered.
    */
   for (int i = 0; i < ARRAY_SIZE(psbc_tess_order); ++i) {
      const mesa_shader_stage s = psbc_tess_order[i];
      const mesa_shader_stage next = stages[s].info.next_stage;
      if (!stages[s].nir || next == MESA_SHADER_NONE || !stages[next].nir)
         continue;

      psbc_tess_fill_linked_io_info(&stages[s], &stages[next]);
   }
}

PsbcResult psbc_compile_nir_tessellation_pipeline(
    const nir_shader* vs_nir, const nir_shader* tcs_nir, const nir_shader* tes_nir,
    const nir_shader* gs_nir,
    const PsbcTessellationCompileOptions* options, PsbcTessellationOutput* out
) {
    if (!out)
        return PSBC_RESULT_INVALID_ARGUMENT;
    const PsbcTessellationOutput empty = {0};
    if (memcmp(out, &empty, sizeof(*out)))
        return PSBC_RESULT_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!vs_nir || !tcs_nir || !tes_nir || !options ||
        !options->input_patch_vertices || options->input_patch_vertices > 32 ||
        !options->offchip_workgroup_capacity_dwords ||
        options->offchip_workgroup_capacity_dwords > UINT32_MAX / 4)
        return PSBC_RESULT_INVALID_ARGUMENT;
    if (vs_nir->info.stage != MESA_SHADER_VERTEX ||
        tcs_nir->info.stage != MESA_SHADER_TESS_CTRL ||
        tes_nir->info.stage != MESA_SHADER_TESS_EVAL ||
        (gs_nir && gs_nir->info.stage != MESA_SHADER_GEOMETRY))
        return PSBC_RESULT_UNSUPPORTED_STAGE;

    /* Check every assert prerequisite before calling RADV's actual merge.
     * Unspecified fields in either stage are legal; no pre-merge required. */
    struct shader_info tcs_info = tcs_nir->info, tes_info = tes_nir->info;
#define PSBC_TESS_CONFLICT(field) (tcs_info.tess.field && tes_info.tess.field && \
                                  tcs_info.tess.field != tes_info.tess.field)
    if (PSBC_TESS_CONFLICT(tcs_vertices_out) || PSBC_TESS_CONFLICT(spacing) ||
        PSBC_TESS_CONFLICT(_primitive_mode))
        return PSBC_RESULT_INVALID_ARGUMENT;
#undef PSBC_TESS_CONFLICT
    psbc_merge_tess_info(&tes_info, &tcs_info);
    if (!tes_info.tess.tcs_vertices_out ||
        tes_info.tess.tcs_vertices_out > 32 ||
        (tes_info.tess._primitive_mode != TESS_PRIMITIVE_TRIANGLES &&
         tes_info.tess._primitive_mode != TESS_PRIMITIVE_QUADS &&
         tes_info.tess._primitive_mode != TESS_PRIMITIVE_ISOLINES) ||
        (tes_info.tess.spacing != TESS_SPACING_EQUAL &&
         tes_info.tess.spacing != TESS_SPACING_FRACTIONAL_ODD &&
         tes_info.tess.spacing != TESS_SPACING_FRACTIONAL_EVEN))
        return PSBC_RESULT_INVALID_ARGUMENT;
    if (!psbc_descriptor_options_valid(&options->vertex) ||
        !psbc_tess_vertex_inputs_match(vs_nir, &options->vertex))
        return PSBC_RESULT_INVALID_ARGUMENT;
    for (unsigned i = 0; i < options->vertex.descriptor_binding_count; ++i) {
        PsbcDescriptorType type = options->vertex.descriptor_bindings[i].type;
        if (!options->vertex.gallium_buffer_arrays ||
            (type != PSBC_DESCRIPTOR_UNIFORM_BUFFER && type != PSBC_DESCRIPTOR_STORAGE_BUFFER &&
             type != PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER &&
             type != PSBC_DESCRIPTOR_STORAGE_IMAGE))
            return PSBC_RESULT_INVALID_ARGUMENT;
    }
    const nir_shader* inputs[] = {vs_nir, tcs_nir, tes_nir, gs_nir};
    const unsigned stage_count = gs_nir ? 4 : 3;
    if (options->vertex.ps5_global_streamout && !inputs[stage_count - 1]->xfb_info)
        return PSBC_RESULT_INVALID_ARGUMENT;
    for (unsigned i = 0; i < stage_count; ++i)
        if (!psbc_tess_input_supported(inputs[i], options->vertex.gallium_buffer_arrays,
                                      options->vertex.ps5_global_streamout && i == stage_count - 1))
            return PSBC_RESULT_INVALID_ARGUMENT;

    PsbcResult result = PSBC_RESULT_COMPILE_NIR;
    PsbcTessellationOutput pair = {0};
    struct radv_shader_stage stages[MESA_VULKAN_SHADER_STAGES] = {0};
    struct radv_shader_binary* binaries[2] = {0};
    struct ac_compiler_info ac = {0};
    setup_ac_info(&ac, GFX10_3);
    ac.hs_offchip_workgroup_dw_size = options->offchip_workgroup_capacity_dwords;
    struct radv_compiler_info ci = {.ac = &ac};
    psbc_setup_descriptor_sizes(&ci);
    ci.key.family = ci.debug.family = CHIP_NAVI21;
    ci.key.ge_wave_size = 64;
    ci.key.ps_wave_size = 32;
    ci.key.use_ngg = true;
    ci.key.ps5_global_streamout = options->vertex.ps5_global_streamout;
    ci.key.ps5_global_primitive_query = options->vertex.ps5_global_primitive_query;
    ci.key.primitives_generated_query = options->vertex.ps5_global_primitive_query;
    ci.hw.address32_hi = options->address32_hi;
    radv_get_nir_options(&ci);
    struct radv_graphics_state_key gfx = {0};
    gfx.ts.patch_control_points = options->input_patch_vertices;
    struct radv_shader_stage* vs = &stages[MESA_SHADER_VERTEX];
    struct radv_shader_stage* hs = &stages[MESA_SHADER_TESS_CTRL];
    struct radv_shader_stage* tes = &stages[MESA_SHADER_TESS_EVAL];
    struct radv_shader_stage* gs = &stages[MESA_SHADER_GEOMETRY];
    const mesa_shader_stage next[] = {
        MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL,
        gs_nir ? MESA_SHADER_GEOMETRY : MESA_SHADER_FRAGMENT,
        MESA_SHADER_FRAGMENT
    };

    struct radv_shader_layout layout = {0};
    _Alignas(struct radv_descriptor_set_layout)
        uint8_t descriptor_set0_storage[sizeof(struct radv_descriptor_set_layout) +
            PSBC_MAX_DESCRIPTOR_BINDINGS * sizeof(struct radv_descriptor_set_binding_layout)] = {0};
    psbc_descriptor_layout(&options->vertex, descriptor_set0_storage, &layout);
    psbc_init();
    for (unsigned i = 0; i < stage_count; ++i) {
        struct radv_shader_stage* s = &stages[psbc_tess_order[i]];
        s->stage = psbc_tess_order[i];
        s->next_stage = next[i];
        PsbcCompileOptions opts = options->vertex;
        if (i)
            opts.vertex_attribute_count = 0;
        opts.target = PSBC_TARGET_PS5;
        opts.optimise = true;
        opts.address32_hi = options->address32_hi;
        opts.stage = i == 0 ? PSBC_STAGE_VERTEX :
                     i == 1 ? PSBC_STAGE_TESS_CTRL :
                     i == 2 ? PSBC_STAGE_TESS_EVAL : PSBC_STAGE_GEOMETRY;
        if (!prepare_stage_nir(&ci, s, NULL, 0, inputs[i], &opts))
            goto cleanup;
        /* The explicit pipeline owns adjacency, including an inserted default
         * TCS. Preserve caller NIR, but replace its original program links. */
        s->nir->info.next_stage = next[i];
        s->nir->info.prev_stage = i ? psbc_tess_order[i - 1] : MESA_SHADER_NONE;
        s->layout = layout;
        radv_nir_shader_info_init(s->stage, next[i], &s->info);
    }
    hs->nir->info.tess = tcs_info.tess;
    tes->nir->info.tess = tes_info.tess;
    /* Only the final pre-raster stage owns NGG. */
    (gs_nir ? gs : tes)->info.is_ngg = true;
    if (gs_nir) {
        NIR_PASS(_, gs->nir, nir_lower_gs_intrinsics,
                 nir_lower_gs_intrinsics_per_stream |
                 nir_lower_gs_intrinsics_count_primitives |
                 nir_lower_gs_intrinsics_count_vertices_per_primitive |
                 nir_lower_gs_intrinsics_overwrite_incomplete);
        NIR_PASS(_, gs->nir, nir_lower_vars_to_ssa);
    }
    for (unsigned i = 0; i < stage_count; ++i) {
        nir_shader* n = stages[psbc_tess_order[i]].nir;
        NIR_PASS(_, n, nir_lower_io_to_scalar, nir_var_shader_in | nir_var_shader_out, NULL, NULL);
        NIR_PASS(_, n, nir_opt_copy_prop);
        NIR_PASS(_, n, nir_opt_constant_folding);
    }
    psbc_tess_link_varyings(stages, ac.gfx_level);
    nir_tcs_info gathered;
    ac_nir_tess_io_info io;
    nir_gather_tcs_info(hs->nir, &gathered, hs->nir->info.tess._primitive_mode,
                       hs->nir->info.tess.spacing);
    ac_nir_get_tess_io_info(hs->nir, &gathered, ~0ull, ~0u, NULL, true, &io);
    if (psbc_tess_output_bytes(&io, tes_info.tess.tcs_vertices_out, 1) >
        ac.hs_offchip_workgroup_dw_size * 4) {
        result = PSBC_RESULT_INVALID_ARGUMENT;
        goto cleanup;
    }
    if (!psbc_apply_vertex_input_state(&options->vertex, &gfx)) {
        result = PSBC_RESULT_INVALID_ARGUMENT;
        goto cleanup;
    }
    for (unsigned i = 0; i < stage_count; ++i) {
        struct radv_shader_stage* s = &stages[psbc_tess_order[i]];
        radv_nir_shader_info_pass(&ci, s->nir, &s->layout, &s->key, &gfx,
                                 RADV_PIPELINE_GRAPHICS, false, &s->info);
        if (options->vertex.descriptor_binding_count)
            s->info.desc_set_used_mask |= 1u;
    }
    radv_nir_shader_info_link(&ci, &gfx, stages);
    if (options->vertex.descriptor_binding_count)
        for (unsigned i = 0; i < stage_count; ++i)
            stages[psbc_tess_order[i]].info.force_indirect_descriptors = false;
    if (!hs->info.num_tess_patches || hs->info.num_tess_patches != tes->info.num_tess_patches ||
        psbc_tess_output_bytes(&hs->info.tcs.io_info,
                               hs->info.tcs.tcs_vertices_out,
                               hs->info.num_tess_patches) >
            ac.hs_offchip_workgroup_dw_size * 4 ||
        !vs->info.vs.as_ls || vs->info.is_ngg || hs->info.is_ngg ||
        tes->info.is_ngg == (gs_nir != NULL) ||
        (gs_nir && !gs->info.is_ngg) ||
        hs->info.tcs.lds_size > ac.lds_size_per_workgroup) {
        fprintf(stderr, "psbc: tessellation link invariant failed gs=%u hs-patches=%u tes-patches=%u flags=%u/%u/%u/%u lds=%u\n",
                gs_nir != NULL, hs->info.num_tess_patches,
                tes->info.num_tess_patches, vs->info.is_ngg,
                hs->info.is_ngg, tes->info.is_ngg, gs->info.is_ngg,
                hs->info.tcs.lds_size);
        result = PSBC_RESULT_INTERNAL_ERROR;
        goto cleanup;
    }

    struct radv_shader_debug_info debug = {0};
    radv_declare_shader_args(&ci, &gfx, hs, MESA_SHADER_VERTEX, &debug);
    hs->info.user_sgprs_locs = hs->args.user_sgprs_locs;
    hs->info.inline_push_constant_mask = hs->args.ac.inline_push_const_mask;
    vs->info.user_sgprs_locs = hs->info.user_sgprs_locs;
    vs->info.inline_push_constant_mask = hs->info.inline_push_constant_mask;
    vs->args = hs->args;
    struct radv_shader_stage *final = gs_nir ? gs : tes;
    radv_declare_shader_args(&ci, &gfx, final,
                             gs_nir ? MESA_SHADER_TESS_EVAL : MESA_SHADER_NONE,
                             &debug);
    final->info.user_sgprs_locs = final->args.user_sgprs_locs;
    final->info.inline_push_constant_mask = final->args.ac.inline_push_const_mask;
    if (gs_nir) {
        tes->args = gs->args;
        tes->info.user_sgprs_locs = gs->info.user_sgprs_locs;
        tes->info.inline_push_constant_mask = gs->info.inline_push_constant_mask;
    }
    if (!hs->args.ac.ring_offsets.used || !final->args.ac.ring_offsets.used ||
        hs->args.user_sgprs_locs.shader_data[AC_UD_SCRATCH_RING_OFFSETS].sgpr_idx != 0 ||
        final->args.user_sgprs_locs.shader_data[AC_UD_SCRATCH_RING_OFFSETS].sgpr_idx != 0 ||
        hs->args.ac.tcs_offchip_layout.used ||
        final->args.ac.tcs_offchip_layout.used != (gs_nir != NULL) ||
        (gs_nir &&
         final->args.user_sgprs_locs.shader_data[AC_UD_TCS_OFFCHIP_LAYOUT].sgpr_idx >=
            final->args.num_user_sgprs)) {
        fprintf(stderr, "psbc: tessellation argument invariant failed gs=%u rings=%u/%u layouts=%u/%u users=%u/%u\n",
                gs_nir != NULL, hs->args.ac.ring_offsets.used,
                final->args.ac.ring_offsets.used,
                hs->args.ac.tcs_offchip_layout.used,
                final->args.ac.tcs_offchip_layout.used,
                hs->args.num_user_sgprs, final->args.num_user_sgprs);
        result = PSBC_RESULT_INTERNAL_ERROR;
        goto cleanup;
    }
    for (unsigned i = 0; i < stage_count; ++i)
        radv_postprocess_nir(&ci, &gfx, &stages[psbc_tess_order[i]]);
    gfx10_get_ngg_info(&ci, &tes->info, gs_nir ? &gs->info : NULL,
                       &final->info.ngg_info);
    final->info.nir_shared_size = final->info.ngg_info.lds_size;
    nir_shader* merged[] = {vs->nir, hs->nir};
    binaries[0] = radv_shader_nir_to_asm(&ci, hs, merged, 2, &gfx);
    if (!binaries[0]) {
        result = PSBC_RESULT_COMPILE_ACO;
        goto cleanup;
    }
    nir_shader *final_merged[] = {tes->nir, gs_nir ? gs->nir : NULL};
    binaries[1] = radv_shader_nir_to_asm(&ci, final, final_merged,
                                         gs_nir ? 2 : 1, &gfx);
    if (!binaries[1]) {
        result = PSBC_RESULT_COMPILE_ACO;
        goto cleanup;
    }
    PsbcShaderOutput* outputs[] = {&pair.hs, &pair.tes};
    for (unsigned i = 0; i < 2; ++i) {
        struct radv_shader_binary_legacy* binary = (void*)binaries[i];
        if (!binary->code_size || !binary->exec_size) {
            result = PSBC_RESULT_COMPILE_ACO;
            goto cleanup;
        }
        outputs[i]->machine_code = malloc(binary->code_size);
        if (!outputs[i]->machine_code) {
            result = PSBC_RESULT_OUT_OF_MEMORY;
            goto cleanup;
        }
        memcpy(outputs[i]->machine_code, binary->data + binary->stats_size, binary->code_size);
        outputs[i]->machine_code_size = binary->code_size;
        PsbcCompileOptions metadata_options = options->vertex;
        metadata_options.target = PSBC_TARGET_PS5;
        metadata_options.stage = i ? (gs_nir ? PSBC_STAGE_GEOMETRY : PSBC_STAGE_TESS_EVAL)
                                  : PSBC_STAGE_TESS_CTRL;
        metadata_options.optimise = true;
        metadata_options.address32_hi = options->address32_hi;
        BuildContext metadata_context = {
            .nir = i ? final->nir : hs->nir,
            .rinfo = &binary->base.info,
            .rargs = i ? &final->args : &hs->args,
            .config = &binary->base.config,
            .gfx_level = GFX10_3,
            .family = CHIP_NAVI21,
            .stage = i ? final->stage : MESA_SHADER_TESS_CTRL,
            .psbc_stage = metadata_options.stage,
            .target = PSBC_TARGET_PS5,
            .ngg = i != 0,
            .address32_hi = options->address32_hi,
            .options = &metadata_options,
        };
        fill_shader_metadata(&metadata_context, &outputs[i]->metadata);
        if (i && gs_nir) {
            uint32_t *stages = &outputs[i]->metadata.linkage_stages_en.value;

            /* fill_shader_metadata sees the final GS in isolation. Restore
             * RADV's tessellation+GS stage selection for the merged pipeline. */
            *stages &= ~(S_028B54_LS_EN(3) | S_028B54_HS_EN(1) |
                         S_028B54_DYNAMIC_HS(1) | S_028B54_ES_EN(3));
            *stages |= S_028B54_LS_EN(V_028B54_LS_STAGE_ON) |
                       S_028B54_HS_EN(1) | S_028B54_DYNAMIC_HS(1) |
                       S_028B54_ES_EN(V_028B54_ES_STAGE_DS);
        }
    }
    pair.runtime = (PsbcTessellationRuntimeInfo) {
        .valid = 1,
        .input_patch_vertices = options->input_patch_vertices,
        .output_patch_vertices = hs->info.tcs.tcs_vertices_out,
        .num_patches = hs->info.num_tess_patches,
        .lds_bytes = hs->info.tcs.lds_size,
        .hs_rsrc2 = binaries[0]->config.rsrc2 |
            S_00B42C_LDS_SIZE_GFX10(ac_shader_encode_lds_size(
                hs->info.tcs.lds_size, GFX10_3, MESA_SHADER_VERTEX)),
        .ls_hs_config = S_028B58_NUM_PATCHES(hs->info.num_tess_patches) |
            S_028B58_HS_NUM_INPUT_CP(options->input_patch_vertices) |
            S_028B58_HS_NUM_OUTPUT_CP(hs->info.tcs.tcs_vertices_out),
        .tf_param = S_028B6C_TYPE(
            tes_info.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES
                ? V_028B6C_TESS_ISOLINE
                : tes_info.tess._primitive_mode == TESS_PRIMITIVE_QUADS
                    ? V_028B6C_TESS_QUAD : V_028B6C_TESS_TRIANGLE) |
            S_028B6C_PARTITIONING(
                tes_info.tess.spacing == TESS_SPACING_FRACTIONAL_ODD
                    ? V_028B6C_PART_FRAC_ODD
                    : tes_info.tess.spacing == TESS_SPACING_FRACTIONAL_EVEN
                        ? V_028B6C_PART_FRAC_EVEN : V_028B6C_PART_INTEGER) |
            S_028B6C_TOPOLOGY(
                tes_info.tess.point_mode ? V_028B6C_OUTPUT_POINT
                : tes_info.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES
                    ? V_028B6C_OUTPUT_LINE
                    : tes_info.tess.ccw ? V_028B6C_OUTPUT_TRIANGLE_CCW
                                        : V_028B6C_OUTPUT_TRIANGLE_CW) |
            S_028B6C_DISTRIBUTION_MODE(V_028B6C_NO_DIST),
        .hs_ring_offsets_sgpr = 0,
        .tes_ring_offsets_sgpr = 0,
        .hs_ring_offsets_register = 0x102,
        .tes_ring_offsets_register = 0x082,
        .final_offchip_layout_valid = gs_nir != NULL,
        .final_offchip_layout_user_data_dword = gs_nir
            ? final->args.user_sgprs_locs.shader_data[AC_UD_TCS_OFFCHIP_LAYOUT].sgpr_idx
            : 0,
        .final_offchip_layout = gs_nir
            ? SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_NUM_PATCHES,
                             hs->info.num_tess_patches) |
              SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_PATCH_VERTICES_IN,
                             hs->info.tcs.tcs_vertices_out - 1) |
              SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_TCS_MEM_ATTRIB_STRIDE,
                             align(hs->info.num_tess_patches *
                                   hs->info.tcs.tcs_vertices_out * 16, 256) / 256) |
              SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_NUM_LS_OUTPUTS,
                             vs->info.vs.num_linked_outputs) |
              SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_NUM_HS_OUTPUTS,
                             hs->info.tcs.io_info.highest_remapped_vram_output) |
              SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_TES_READS_TF,
                             tes->info.tes.reads_tess_factors) |
              SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_PRIMITIVE_MODE,
                             tes->info.tes._primitive_mode)
            : 0,
        .offchip_ring_bytes_per_workgroup =
            options->offchip_workgroup_capacity_dwords * 4,
        .tess_factor_ring_bytes_per_workgroup =
            hs->info.num_tess_patches * 16,
    };
    *out = pair;
    memset(&pair, 0, sizeof(pair));
    result = PSBC_RESULT_OK;
cleanup:
    free(binaries[0]);
    free(binaries[1]);
    for (unsigned i = 0; i < stage_count; ++i)
        ralloc_free(stages[psbc_tess_order[i]].nir);
    psbc_free_tessellation_output(&pair);
    psbc_shutdown();
    return result;
}

void psbc_free_tessellation_output(PsbcTessellationOutput* out) {
    if (!out)
        return;
    psbc_free_output(&out->hs);
    psbc_free_output(&out->tes);
    memset(out, 0, sizeof(*out));
}

static nir_mem_access_size_align compute_private_access_size(
    nir_intrinsic_op intrin, uint8_t bytes, uint8_t bit_size,
    uint32_t align, uint32_t align_offset, bool offset_is_const,
    enum gl_access_qualifier access, const void *data)
{
    (void)intrin; (void)bytes; (void)bit_size; (void)align;
    (void)align_offset; (void)offset_is_const; (void)access; (void)data;
    return (nir_mem_access_size_align){
        .num_components = 1,
        .bit_size = 32,
        .align = 4,
        .shift = nir_mem_access_shift_method_scalar,
    };
}

struct compute_private_lowering {
    unsigned stride;
    unsigned shared_base;
    bool shared;
};

static bool lower_compute_private(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
    const struct compute_private_lowering *state = data;
    const unsigned stride = state->stride;
    const bool store = intr->intrinsic == nir_intrinsic_store_scratch;
    if (!store && intr->intrinsic != nir_intrinsic_load_scratch)
        return false;
    b->cursor = nir_before_instr(&intr->instr);
    nir_def *local = nir_load_local_invocation_id(b);
    const uint16_t *size = b->shader->info.workgroup_size;
    nir_def *li = nir_iadd(b, nir_channel(b, local, 0), nir_imul_imm(b,
        nir_iadd(b, nir_channel(b, local, 1), nir_imul_imm(b,
            nir_channel(b, local, 2), size[1])), size[0]));
    /* Bound even undefined source indices to this invocation's owned region. */
    nir_def *offset = nir_umin(b, intr->src[store ? 1 : 0].ssa,
                               nir_imm_int(b, stride - 4));
    if (state->shared) {
        nir_def *address = nir_iadd_imm(b,
            nir_iadd(b, nir_imul_imm(b, li, stride), offset), state->shared_base);
        if (store) {
            nir_store_shared(b, intr->src[0].ssa, address, .align_mul=1,
                             .write_mask=1, .access=ACCESS_VOLATILE);
        } else {
            nir_barrier(b, .memory_scope=SCOPE_INVOCATION,
                        .memory_semantics=NIR_MEMORY_ACQ_REL,
                        .memory_modes=nir_var_mem_shared);
            nir_def_rewrite_uses(&intr->def, nir_load_shared(b, 1, 32, address,
                                 .align_mul=1, .access=ACCESS_VOLATILE));
        }
        nir_instr_remove(&intr->instr);
        return true;
    }
    nir_def *group = nir_load_workgroup_id(b);
    nir_def *grid = nir_load_num_workgroups(b);
    nir_def *gi = nir_iadd(b, nir_channel(b, group, 0), nir_imul(b,
        nir_channel(b, grid, 0), nir_iadd(b, nir_channel(b, group, 1),
            nir_imul(b, nir_channel(b, grid, 1), nir_channel(b, group, 2)))));
    nir_def *id = nir_iadd(b, li, nir_imul_imm(b, gi, size[0]*size[1]*size[2]));
    nir_def *base = nir_pack_64_2x32(b, nir_load_scalar_arg_amd(b, 2, .base=0));
    nir_def *address = nir_iadd(b, base, nir_u2u64(b,
        nir_iadd(b, nir_imul_imm(b, id, stride), offset)));
    if (store) {
        nir_store_global(b, intr->src[0].ssa, address, .align_mul=1,
                         .write_mask=1, .access=ACCESS_VOLATILE);
    } else {
        nir_barrier(b, .memory_scope=SCOPE_INVOCATION,
                    .memory_semantics=NIR_MEMORY_ACQ_REL, .memory_modes=nir_var_mem_global);
        nir_def_rewrite_uses(&intr->def, nir_load_global(b, 1, 32, address,
                             .align_mul=1, .access=ACCESS_VOLATILE));
    }
    nir_instr_remove(&intr->instr);
    return true;
}

static bool compute_private_supported(nir_shader *nir)
{
    unsigned count = 1;
    if (nir->info.workgroup_size_variable || !nir->scratch_size ||
        (nir->scratch_size & 3) || nir->scratch_size > 4096)
        return false;
    for (unsigned i=0; i<3; ++i) {
        if (!nir->info.workgroup_size[i] || nir->info.workgroup_size[i] > 1024/count)
            return false;
        count *= nir->info.workgroup_size[i];
    }
    nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl)
        nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            bool store = intr->intrinsic == nir_intrinsic_store_scratch;
            if (!store && intr->intrinsic != nir_intrinsic_load_scratch) continue;
            nir_def *value = store ? intr->src[0].ssa : &intr->def;
            if (value->bit_size != 32 || value->num_components != 1 ||
                intr->src[store ? 1 : 0].ssa->bit_size != 32)
                return false;
        }
    return true;
}

struct psbc_compile_scratch {
    struct ac_compiler_info ac_info;
    struct radv_compiler_info compiler_info;
    struct radv_shader_stage stage;
    struct radv_shader_stage previous;
    struct radv_shader_stage stages[MESA_VULKAN_SHADER_STAGES];
    struct radv_shader_layout layout;
    _Alignas(struct radv_descriptor_set_layout)
        uint8_t descriptor_set0_storage[
            sizeof(struct radv_descriptor_set_layout) +
            PSBC_MAX_DESCRIPTOR_BINDINGS *
                sizeof(struct radv_descriptor_set_binding_layout)];
    struct radv_graphics_state_key gfx_state;
};

static PsbcResult psbc_compile_impl_inner(
    const uint32_t*       spirv,
    size_t                spirv_size,
    const nir_shader*     input_nir,
    const uint32_t*       previous_spirv,
    size_t                previous_spirv_size,
    const nir_shader*     previous_input_nir,
    const PsbcCompileOptions* opts,
    PsbcShaderOutput*     out,
    struct psbc_compile_scratch* scratch
) {
    struct ac_compiler_info *ac_info = &scratch->ac_info;
    struct radv_compiler_info *compiler_info = &scratch->compiler_info;
    struct radv_shader_stage *stage = &scratch->stage;
    struct radv_shader_stage *previous = &scratch->previous;
    struct radv_shader_layout *layout = &scratch->layout;
    uint8_t *descriptor_set0_storage = scratch->descriptor_set0_storage;
    struct radv_graphics_state_key *gfx_state = &scratch->gfx_state;
    if ((!spirv && !input_nir) || !opts || !out)
        return PSBC_RESULT_INTERNAL_ERROR;

    memset(out, 0, sizeof(*out));

    /* Validate SPIR-V header when the caller did not supply NIR. */
    if (!input_nir && (spirv_size < 4 || spirv[0] != 0x07230203u))
        return PSBC_RESULT_INVALID_SPIRV;

    const bool paired_geometry = previous_spirv || previous_input_nir;
    mesa_shader_stage mesa_stage = psbc_to_mesa_stage(opts->stage);
    if (mesa_stage == MESA_SHADER_NONE)
        return PSBC_RESULT_UNSUPPORTED_STAGE;
    if (opts->gallium_buffer_arrays &&
        (!input_nir || input_nir->info.stage != mesa_stage ||
         mesa_stage > MESA_SHADER_COMPUTE || previous_spirv))
        return PSBC_RESULT_UNSUPPORTED_STAGE;
    if (opts->compute_buffer_spills &&
        (opts->target != PSBC_TARGET_PS5 || mesa_stage != MESA_SHADER_COMPUTE))
        return PSBC_RESULT_UNSUPPORTED_STAGE;
    if (paired_geometry &&
        (mesa_stage != MESA_SHADER_GEOMETRY || !opts->ngg ||
         opts->target != PSBC_TARGET_PS5))
        return PSBC_RESULT_UNSUPPORTED_STAGE;
    if (opts->ps5_global_streamout && !paired_geometry)
        return PSBC_RESULT_UNSUPPORTED_STAGE;
    if (opts->primitive_id_per_primitive &&
        (opts->target != PSBC_TARGET_PS5 || mesa_stage != MESA_SHADER_FRAGMENT))
        return PSBC_RESULT_UNSUPPORTED_STAGE;
    if (previous_input_nir &&
        previous_input_nir->info.stage != MESA_SHADER_VERTEX)
        return PSBC_RESULT_UNSUPPORTED_STAGE;
    if (previous_spirv &&
        (previous_spirv_size < 4 || previous_spirv[0] != 0x07230203u))
        return PSBC_RESULT_INVALID_SPIRV;
    if (opts->ngg &&
        (opts->target != PSBC_TARGET_PS5 ||
         (mesa_stage != MESA_SHADER_VERTEX && !paired_geometry)))
        return PSBC_RESULT_UNSUPPORTED_STAGE;
    if (opts->descriptor_binding_count > PSBC_MAX_DESCRIPTOR_BINDINGS)
        return PSBC_RESULT_INTERNAL_ERROR;
    if ((opts->color_is_int8 | opts->color_is_int10) & ~UINT32_C(0xff))
        return PSBC_RESULT_INTERNAL_ERROR;
    for (unsigned i = 0; i < 8; ++i)
        if (((opts->spi_shader_col_format >> (4 * i)) & 0xf) >
            V_028714_SPI_SHADER_32_ABGR)
            return PSBC_RESULT_INTERNAL_ERROR;
    if (!psbc_descriptor_options_valid(opts))
        return PSBC_RESULT_INTERNAL_ERROR;

    /* Ensure library is initialized */
    psbc_init();

    /* Select GFX level and family based on target */
    enum amd_gfx_level gfxlevel;
    enum radeon_family chipfamily;
    bool neo = false;
    setup_target(opts->target, &gfxlevel, &chipfamily, &neo);

    /* Construct ac_compiler_info for the target GPU */
    setup_ac_info(ac_info, gfxlevel);
    if (opts->force_accelerated_dot)
        ac_info->has_accelerated_dot_product = true;

    /* Construct radv_compiler_info */
    compiler_info->ac = ac_info;
    compiler_info->spirv_caps.Shader = true;
    compiler_info->spirv_caps.Geometry = true;
    compiler_info->spirv_caps.TransformFeedback = true;
    compiler_info->spirv_caps.DotProduct = true;
    compiler_info->spirv_caps.DotProductInput4x8BitPacked = true;
    if (opts->force_accelerated_dot) {
        compiler_info->spirv_caps.Int16 = true;
        compiler_info->spirv_caps.DotProductInputAll = true;
    }
    compiler_info->hw.address32_hi = opts->address32_hi;
    /* The native CS owner supplies three grid dimensions directly, not a
     * Vulkan dispatch-parameter pointer with a separate lifetime. */
    compiler_info->key.load_grid_size_from_user_sgpr = mesa_stage == MESA_SHADER_COMPUTE;
    psbc_setup_descriptor_sizes(compiler_info);
    compiler_info->key.ge_wave_size = 64;
    compiler_info->key.ps_wave_size = (gfxlevel >= GFX10_3) ? 32 : 64;
    compiler_info->key.cs_wave_size = (gfxlevel >= GFX10_3) ? 32 : 64;
    compiler_info->key.rt_wave_size = 64;
    compiler_info->key.family = chipfamily;
    compiler_info->key.load_grid_size_from_user_sgpr = (gfxlevel >= GFX10_3);
    compiler_info->key.use_ngg = opts->ngg;
    compiler_info->key.ps5_global_streamout = opts->ps5_global_streamout;
    compiler_info->key.ps5_global_primitive_query = opts->ps5_global_primitive_query;
    compiler_info->key.primitives_generated_query = opts->ps5_global_primitive_query;
    compiler_info->key.ps5_buffer_scratch = opts->compute_buffer_spills;
    /* ACO uses debug.family for disassembly and init_program assertion */
    compiler_info->debug.family = chipfamily;

    /* Initialize NIR options for all stages */
    radv_get_nir_options(compiler_info);

    /* Construct radv_shader_stage */
    stage->stage = mesa_stage;
    stage->key.keep_executable_info = getenv("PSBC_DEBUG_DISASM") != NULL;
    /* Set next_stage based on the pipeline graph.
     * For standalone compilation we assume the simplest pipeline:
     *   VS → FS, VS → HS → DS → FS, VS → GS → FS
     */
    switch (mesa_stage) {
    case MESA_SHADER_VERTEX:    stage->next_stage = MESA_SHADER_FRAGMENT;     break;
    case MESA_SHADER_TESS_CTRL: stage->next_stage = MESA_SHADER_TESS_EVAL;    break;
    case MESA_SHADER_TESS_EVAL: stage->next_stage = MESA_SHADER_FRAGMENT;     break;
    case MESA_SHADER_GEOMETRY:  stage->next_stage = MESA_SHADER_FRAGMENT;     break;
    default:                    stage->next_stage = MESA_SHADER_NONE;         break;
    }
    if (input_nir && input_nir->info.stage != mesa_stage) {
        psbc_shutdown();
        return PSBC_RESULT_UNSUPPORTED_STAGE;
    }
    debug_stage(input_nir ? "import-nir-begin" : "import-spirv-begin");
    nir_shader* nir = prepare_stage_nir(
        compiler_info, stage, spirv, spirv_size, input_nir, opts
    );
    if (!nir) {
        fprintf(stderr, "PSBC NIR failure: prepare-stage\n");
        psbc_shutdown();
        return PSBC_RESULT_COMPILE_NIR;
    }
    debug_stage(input_nir ? "import-nir-end" : "import-spirv-end");
    debug_shader_io(input_nir ? "input-NIR" : "SPIR-V", nir, NULL);
    debug_shader_io("lowered-io", nir, NULL);

    unsigned compute_private_stride = 0;
    if (opts->compute_private_buffer && nir->scratch_size) {
        const nir_lower_mem_access_bit_sizes_options private_access = {
            .callback = compute_private_access_size,
            .modes = nir_var_shader_temp | nir_var_function_temp,
        };
        NIR_PASS(_, nir, nir_lower_mem_access_bit_sizes, &private_access);
        if (opts->target != PSBC_TARGET_PS5 || mesa_stage != MESA_SHADER_COMPUTE ||
            !compute_private_supported(nir)) {
            fprintf(stderr, "PSBC NIR failure: private-shape\n");
            ralloc_free(nir);
            psbc_shutdown();
            return PSBC_RESULT_COMPILE_NIR;
        }
        struct compute_private_lowering lowering = {.stride=nir->scratch_size};
        if (opts->compute_buffer_spills) {
            unsigned invocations = 1;
            for (unsigned i=0; i<3; ++i)
                invocations *= nir->info.workgroup_size[i];
            lowering.shared_base = (nir->info.shared_size + 15u) & ~15u;
            uint64_t required = (uint64_t)lowering.stride * invocations;
            lowering.shared = lowering.shared_base <= 65536u &&
                              required <= 65536u - lowering.shared_base;
            if (!lowering.shared) {
                fprintf(stderr, "PSBC NIR failure: private-shared-capacity\n");
                ralloc_free(nir);
                psbc_shutdown();
                return PSBC_RESULT_COMPILE_NIR;
            }
            nir->info.shared_size = lowering.shared_base + required;
        }
        if (!lowering.shared)
            compute_private_stride = lowering.stride;
        nir_shader_intrinsics_pass(nir, lower_compute_private,
                                   nir_metadata_control_flow, &lowering);
        nir->scratch_size = 0;
    }
    if (opts->compute_buffer_spills && nir->scratch_size) {
        fprintf(stderr, "PSBC NIR failure: residual-scratch\n");
        ralloc_free(nir);
        psbc_shutdown();
        return PSBC_RESULT_COMPILE_NIR;
    }
    const bool dual_source_blend =
        mesa_stage == MESA_SHADER_FRAGMENT &&
        (nir->info.outputs_written &
         BITFIELD64_BIT(FRAG_RESULT_DUAL_SRC_BLEND));
    nir_shader* previous_nir = NULL;
    if (paired_geometry) {
        previous->stage = MESA_SHADER_VERTEX;
        previous->next_stage = MESA_SHADER_GEOMETRY;
        previous_nir = prepare_stage_nir(
            compiler_info, previous, previous_spirv, previous_spirv_size,
            previous_input_nir, opts
        );
        if (!previous_nir) {
            fprintf(stderr, "PSBC NIR failure: previous-stage\n");
            ralloc_free(nir);
            psbc_shutdown();
            return PSBC_RESULT_COMPILE_NIR;
        }
        debug_shader_io(previous_input_nir ? "previous-NIR" :
                                               "previous-SPIR-V",
                        previous_nir, NULL);
    }

    /* Shader info + args + postprocess */
    stage->nir = nir;
    radv_nir_shader_info_init(stage->stage, stage->next_stage, &stage->info);
    stage->info.is_ngg = opts->ngg;
    if (paired_geometry) {
        radv_nir_shader_info_init(previous->stage, previous->next_stage,
                                  &previous->info);
        previous->info.is_ngg = true;
        NIR_PASS(_, nir, nir_lower_gs_intrinsics,
                 nir_lower_gs_intrinsics_per_stream |
                 nir_lower_gs_intrinsics_count_primitives |
                 nir_lower_gs_intrinsics_count_vertices_per_primitive |
                 nir_lower_gs_intrinsics_overwrite_incomplete);
        NIR_PASS(_, nir, nir_lower_vars_to_ssa);

        /* Both shaders are passed to ACO as one merged program.  Mark their
         * existing shared driver-location interface accordingly; otherwise
         * RADV selects the ABI for independently compiled merged halves. */
        const unsigned linked_slots = util_bitcount64(nir->info.inputs_read);
        previous->info.vs.num_linked_outputs = linked_slots;
        previous->info.outputs_linked = true;
        stage->info.gs.num_linked_inputs = linked_slots;
        stage->info.inputs_linked = true;
    }

    psbc_descriptor_layout(opts, descriptor_set0_storage, layout);
    stage->layout = *layout;
    if (paired_geometry)
        previous->layout = *layout;
    gfx_state->rs.provoking_vtx_last = opts->provoking_vtx_last;
    if (opts->rasterization_samples != 0 &&
        opts->rasterization_samples != 1 &&
        opts->rasterization_samples != 2 &&
        opts->rasterization_samples != 4 &&
        opts->rasterization_samples != 8) {
        if (previous_nir)
            ralloc_free(previous_nir);
        ralloc_free(nir);
        psbc_shutdown();
        return PSBC_RESULT_INTERNAL_ERROR;
    }
    gfx_state->ms.rasterization_samples = opts->rasterization_samples;
    switch (opts->primitive_type) {
    case 0:
        break;
    case 1:
        gfx_state->ia.topology = V_008958_DI_PT_POINTLIST;
        break;
    case 2:
        gfx_state->ia.topology = V_008958_DI_PT_LINELIST;
        break;
    case 3:
        gfx_state->ia.topology = V_008958_DI_PT_LINESTRIP;
        break;
    case 4:
        gfx_state->ia.topology = V_008958_DI_PT_TRILIST;
        break;
    case 5:
        gfx_state->ia.topology = V_008958_DI_PT_TRIFAN;
        break;
    case 6:
        gfx_state->ia.topology = V_008958_DI_PT_TRISTRIP;
        break;
    case 10:
        gfx_state->ia.topology = V_008958_DI_PT_LINELIST_ADJ;
        break;
    case 11:
        gfx_state->ia.topology = V_008958_DI_PT_LINESTRIP_ADJ;
        break;
    case 12:
        gfx_state->ia.topology = V_008958_DI_PT_TRILIST_ADJ;
        break;
    case 13:
        gfx_state->ia.topology = V_008958_DI_PT_TRISTRIP_ADJ;
        break;
    default:
        if (previous_nir)
            ralloc_free(previous_nir);
        ralloc_free(nir);
        psbc_shutdown();
        return PSBC_RESULT_INTERNAL_ERROR;
    }
    /* The frontend keys these exports by framebuffer format. Keep the legacy
     * defaults for standalone callers that do not provide framebuffer state. */
    gfx_state->ps.epilog.spi_shader_col_format = opts->spi_shader_col_format
        ? opts->spi_shader_col_format : UINT32_C(0x99999999);
    gfx_state->ps.epilog.color_is_int8 = opts->spi_shader_col_format
        ? opts->color_is_int8 : 0xff;
    gfx_state->ps.epilog.color_is_int10 = opts->spi_shader_col_format
        ? opts->color_is_int10 : 0;
    gfx_state->ps.has_epilog = false;
    if (dual_source_blend) {
        gfx_state->ps.epilog.mrt0_is_dual_src = true;
        /* Both sources feed MRT0, including its precision and clamp rules. */
        unsigned format = opts->spi_shader_col_format
            ? opts->spi_shader_col_format & 0xf : V_028714_SPI_SHADER_FP16_ABGR;
        gfx_state->ps.epilog.spi_shader_col_format = format * 0x11;
        gfx_state->ps.epilog.color_is_int8 = opts->spi_shader_col_format
            ? (opts->color_is_int8 & 1) * 3 : 0;
        gfx_state->ps.epilog.color_is_int10 = opts->spi_shader_col_format
            ? (opts->color_is_int10 & 1) * 3 : 0;
    }
    if (!psbc_apply_vertex_input_state(opts, gfx_state)) {
        if (previous_nir)
            ralloc_free(previous_nir);
        ralloc_free(nir);
        psbc_shutdown();
        return PSBC_RESULT_INTERNAL_ERROR;
    }

    /* RADV normally lowers fragment coordinates before collecting shader
     * info.  Keep standalone compilation in that order so the PS argument
     * map enables POS_FIXED_PT when the optimization selects it. */
    if (mesa_stage == MESA_SHADER_FRAGMENT)
        NIR_PASS(_, nir, radv_nir_lower_opt_fs_frag_pos,
                 gfx_state->vrs_may_be_enabled,
                 gfx_state->ms.sample_shading_enable ||
                    nir->info.fs.uses_sample_shading);

    radv_nir_shader_info_pass(
        compiler_info, nir, layout, &stage->key, gfx_state,
        mesa_stage == MESA_SHADER_COMPUTE ? RADV_PIPELINE_COMPUTE : RADV_PIPELINE_GRAPHICS,
        false, &stage->info
    );
    if (paired_geometry) {
        radv_nir_shader_info_pass(
            compiler_info, previous_nir, layout, &previous->key, gfx_state,
            RADV_PIPELINE_GRAPHICS, false, &previous->info
        );
    }
    /* Legacy Gallium texture indices carry no Vulkan deref for RADV's info
     * pass to discover. The explicit PSBC layout still requires set 0. */
    if (opts->descriptor_binding_count)
        stage->info.desc_set_used_mask |= 1u;
    if (paired_geometry && opts->descriptor_binding_count)
        previous->info.desc_set_used_mask |= 1u;
    debug_stage("shader-info-end");
    debug_shader_io("info", nir, &stage->info);
    if (paired_geometry)
        debug_shader_io("previous-info", previous_nir, &previous->info);

    if (opts->ngg) {
        struct radv_shader_stage *stages = scratch->stages;
        stages[mesa_stage] = *stage;
        if (paired_geometry)
            stages[MESA_SHADER_VERTEX] = *previous;
        radv_nir_shader_info_link(compiler_info, gfx_state, stages);
        stage->info = stages[mesa_stage].info;
        if (paired_geometry)
            previous->info = stages[MESA_SHADER_VERTEX].info;
        /* Standalone linking conservatively adds PrimitiveID without an FS.
         * Drop only the implicit, final per-primitive parameter when the
         * caller has proved it dead. Keep explicit outputs and unknown
         * consumers unchanged, and update both code lowering and metadata. */
        if (mesa_stage == MESA_SHADER_VERTEX &&
            !(nir->info.outputs_written & VARYING_BIT_PRIMITIVE_ID) &&
            stage->info.outinfo.export_prim_id_per_primitive &&
            stage->info.outinfo.prim_param_exports == 1 &&
            stage->info.outinfo.vs_output_param_offset[VARYING_SLOT_PRIMITIVE_ID] ==
                stage->info.outinfo.param_exports) {
            if (opts->omit_implicit_primitive_id) {
                stage->info.outinfo.export_prim_id_per_primitive = false;
                stage->info.outinfo.prim_param_exports = 0;
                stage->info.outinfo.vs_output_param_offset[VARYING_SLOT_PRIMITIVE_ID] =
                    AC_EXP_PARAM_UNDEFINED;
            } else if (opts->target == PSBC_TARGET_PS5) {
                /* Native traces have correct counts/offsets but undefined
                 * per-primitive ID values. Use Mesa's per-vertex LDS route:
                 * retain the export slot, disable passthrough, and let the
                 * existing NGG pass size LDS and insert its barrier. */
                stage->info.outinfo.export_prim_id_per_primitive = false;
                stage->info.outinfo.prim_param_exports = 0;
                stage->info.outinfo.export_prim_id = true;
                ++stage->info.outinfo.param_exports;
                stage->info.is_ngg_passthrough = false;
            }
        }
        debug_shader_io("linked", nir, &stage->info);
    }

    /* PSBC exposes one explicit set-0 table to its standalone caller.  Do not
     * inherit RADV's pipeline-library indirection for merged shaders: the
     * direct set pointer fits the PS5 merged user-SGPR budget and matches the
     * public metadata/runtime ABI. */
    if (opts->descriptor_binding_count) {
        stage->info.force_indirect_descriptors = false;
        if (paired_geometry)
            previous->info.force_indirect_descriptors = false;
    }

    /* Determine previous stage for shader args declaration.
     * HS/GS need previous_stage=VERTEX so that the merged-pipeline args
     * (prolog_inputs, vertex_buffers, etc.) are declared for the NIR
     * lowering passes. The VS-specific input usage slots are filtered
     * out in buildshaderbinary for non-VS stages. */
    mesa_shader_stage previous_stage = MESA_SHADER_NONE;
    switch (mesa_stage) {
    case MESA_SHADER_TESS_CTRL: previous_stage = MESA_SHADER_VERTEX;    break;
    case MESA_SHADER_TESS_EVAL: previous_stage = MESA_SHADER_TESS_CTRL; break;
    case MESA_SHADER_GEOMETRY:  previous_stage = MESA_SHADER_VERTEX;    break;
    default:                    previous_stage = MESA_SHADER_NONE;      break;
    }

    if (opts->split_vertex_instances && mesa_stage != MESA_SHADER_VERTEX) {
        if (previous_nir)
            ralloc_free(previous_nir);
        ralloc_free(nir);
        psbc_shutdown();
        return PSBC_RESULT_COMPILE_NIR;
    }
    stage->info.vs.needs_instance_id_bias = opts->split_vertex_instances;
    radv_declare_shader_args(
        compiler_info, gfx_state, stage, previous_stage, NULL
    );
    debug_stage("declare-args-end");
    if (compute_private_stride && (!stage->args.ac.ring_offsets.used ||
        stage->args.ac.ring_offsets.arg_index != 0 || stage->args.ac.args[0].size != 2)) {
        fprintf(stderr, "PSBC NIR failure: private-ring-abi\n");
        ralloc_free(nir);
        psbc_shutdown();
        return PSBC_RESULT_COMPILE_NIR;
    }

    stage->info.user_sgprs_locs = stage->args.user_sgprs_locs;
    stage->info.inline_push_constant_mask = stage->args.ac.inline_push_const_mask;
    if (paired_geometry) {
        previous->args = stage->args;
        previous->info.user_sgprs_locs = stage->info.user_sgprs_locs;
        previous->info.inline_push_constant_mask =
            stage->info.inline_push_constant_mask;
    }

    /* For geometry shaders, run the legacy GS lowering pass.
     * On GFX10.3 (PS5), NGG GS requires merging with the previous stage,
     * which is not possible for standalone compilation. We use the legacy
     * GS path instead, which ACO supports on all GFX levels. */
    if (mesa_stage == MESA_SHADER_GEOMETRY && !stage->info.is_ngg) {
        /* Convert emit_vertex to emit_vertex_with_counter */
        NIR_PASS(_, nir, nir_lower_gs_intrinsics,
                 nir_lower_gs_intrinsics_per_stream);
        NIR_PASS(_, nir, nir_lower_vars_to_ssa);

        ac_nir_lower_legacy_gs_options gs_options = {
            .gfx_level = gfxlevel,
            .export_clipdist_mask = 0xff,
            .write_pos_to_clipvertex = true,
            .has_param_exports = stage->info.outinfo.param_exports > 0,
            .disable_streamout = true,
        };
        nir_shader* gs_copy = NULL;
        ac_nir_legacy_gs_info gs_out_info = {0};
        NIR_PASS(_, nir, ac_nir_lower_legacy_gs, &gs_options, &gs_copy, &gs_out_info);
        /* The GS copy shader is discarded for standalone compilation.
         * The PS4 runtime generates the copy shader separately. */
        if (gs_copy)
            ralloc_free(gs_copy);
    }

    if (paired_geometry)
        radv_postprocess_nir(compiler_info, gfx_state, previous);
    radv_postprocess_nir(compiler_info, gfx_state, stage);
    if (opts->split_vertex_instances)
        nir_shader_intrinsics_pass(nir, lower_instance_id_bias,
                                   nir_metadata_control_flow, &stage->args);
    if (mesa_stage == MESA_SHADER_FRAGMENT &&
        opts->target == PSBC_TARGET_PS5)
        nir_shader_intrinsics_pass(nir, lower_flat_input_vertex,
                                   nir_metadata_control_flow, (void*)opts);
    if (mesa_stage == MESA_SHADER_FRAGMENT &&
        opts->target == PSBC_TARGET_PS5 &&
        !split_ps5_mixed_inputs(nir, &stage->info, opts->primitive_id_per_primitive)) {
        if (previous_nir)
            ralloc_free(previous_nir);
        ralloc_free(nir);
        psbc_shutdown();
        return PSBC_RESULT_INTERNAL_ERROR;
    }
    NIR_PASS(_, nir, nir_opt_licm, psbc_licm_speculatable);
    debug_stage("postprocess-end");
    debug_shader_io("postprocess", nir, &stage->info);
    if (getenv("PSBC_DEBUG_NIR"))
        nir_print_shader(nir, stderr);

    /* Snapshot the final flat/interpolated input forms before ACO consumes
     * and may rewrite the NIR. */
    PsbcInputSemantics input_semantics = {.valid = true};
    if (mesa_stage == MESA_SHADER_FRAGMENT &&
        !build_input_semantics(nir, &stage->info, opts->primitive_id_per_primitive,
                               &input_semantics) && (stage->info.ps.prim_id_input || stage->info.ps.has_pcoord)) {
        /* Never package a live built-in consumer with incomplete linkage. */
        ralloc_free(nir);
        psbc_shutdown();
        return PSBC_RESULT_INTERNAL_ERROR;
    }

    if (opts->ngg) {
        gfx10_get_ngg_info(compiler_info,
                           paired_geometry ? &previous->info : &stage->info,
                           paired_geometry ? &stage->info : NULL,
                           &stage->info.ngg_info);
        stage->info.nir_shared_size = stage->info.ngg_info.lds_size;
    }

    /* Compile NIR to GCN ISA via ACO */
    nir_shader* shaders[2] = {nir, NULL};
    unsigned shader_count = 1;
    if (paired_geometry) {
        shaders[0] = previous_nir;
        shaders[1] = nir;
        shader_count = 2;
    }
    if (getenv("PSBC_DEBUG_DISASM"))
        stage->key.keep_executable_info = true;
    struct radv_shader_binary* binary = radv_shader_nir_to_asm(
        compiler_info, stage, shaders, shader_count, gfx_state
    );
    debug_stage("aco-end");

    if (!binary) {
        if (previous_nir)
            ralloc_free(previous_nir);
        ralloc_free(nir);
        psbc_shutdown();
        return PSBC_RESULT_COMPILE_ACO;
    }

    /* Extract code from radv_shader_binary_legacy */
    struct radv_shader_binary_legacy* legacy =
        (struct radv_shader_binary_legacy*)binary;

    if (getenv("PSBC_DEBUG_DISASM")) {
        fprintf(stderr, "PSBC executable code=%u ir=%u disasm=%u\n",
                legacy->code_size, legacy->ir_size, legacy->disasm_size);
        if (legacy->ir_size) {
            const char* ir = (const char*)legacy->data + legacy->stats_size +
                             legacy->code_size;
            fprintf(stderr, "%.*s\n", (int)legacy->ir_size, ir);
        }
    }
    if (getenv("PSBC_DEBUG_DISASM") && legacy->disasm_size) {
        const uint8_t* disasm = legacy->data + legacy->stats_size +
                                legacy->code_size + legacy->ir_size;
        fwrite(disasm, 1, legacy->disasm_size, stderr);
        fputc('\n', stderr);
    }

    /* The data layout in radv_shader_binary_legacy is:
     * [stats | code | ir | disasm | debug_info]
     * Code starts at offset stats_size. */
    const uint32_t* code = (const uint32_t*)(legacy->data + legacy->stats_size);
    uint32_t code_dw = legacy->code_size / sizeof(uint32_t);

    /* Build the PS4/PS5 shader binary */
    const BuildContext buildctx = {
        .rinfo = &binary->info,
        .rargs = &stage->args,
        .config = &binary->config,
        .nir = nir,
        .gfx_level = gfxlevel,
        .family = chipfamily,
        .stage = mesa_stage,
        .psbc_stage = opts->stage,
        .spirv_data = spirv,
        .spirv_size = spirv_size,
        .target = opts->target,
        .ngg = opts->ngg,
        .neo = neo,
        .address32_hi = opts->address32_hi,
        .options = opts,
        .input_semantics = &input_semantics,
    };

    uint8_t* output_data = NULL;
    size_t output_size = 0;
    PsbcResult result = buildshaderbinary(&buildctx, code, code_dw,
                                          &output_data, &output_size);
    if (result != PSBC_RESULT_OK) {
        free(binary);
        if (previous_nir)
            ralloc_free(previous_nir);
        ralloc_free(nir);
        psbc_shutdown();
        return result;
    }

    void* machine_code = malloc(legacy->code_size);
    if (!machine_code) {
        free(output_data);
        free(binary);
        if (previous_nir)
            ralloc_free(previous_nir);
        ralloc_free(nir);
        psbc_shutdown();
        return PSBC_RESULT_OUT_OF_MEMORY;
    }
    memcpy(machine_code, code, legacy->code_size);

    out->data = output_data;
    out->size = output_size;
    out->machine_code = machine_code;
    out->machine_code_size = legacy->code_size;
    fill_shader_metadata(&buildctx, &out->metadata);
    out->metadata.compute_private_stride = compute_private_stride;

    free(binary);
    if (previous_nir)
        ralloc_free(previous_nir);
    ralloc_free(nir);
    psbc_shutdown();

    return result;
}

static PsbcResult psbc_compile_impl(
    const uint32_t*       spirv,
    size_t                spirv_size,
    const nir_shader*     input_nir,
    const uint32_t*       previous_spirv,
    size_t                previous_spirv_size,
    const nir_shader*     previous_input_nir,
    const PsbcCompileOptions* opts,
    PsbcShaderOutput*     out
) {
    if ((!spirv && !input_nir) || !opts || !out)
        return PSBC_RESULT_INTERNAL_ERROR;

    struct psbc_compile_scratch *scratch = calloc(1, sizeof(*scratch));
    if (!scratch) {
        memset(out, 0, sizeof(*out));
        return PSBC_RESULT_OUT_OF_MEMORY;
    }
    PsbcResult result = psbc_compile_impl_inner(
        spirv, spirv_size, input_nir, previous_spirv, previous_spirv_size,
        previous_input_nir, opts, out, scratch
    );
    free(scratch);
    return result;
}

PsbcResult psbc_compile_shader(
    const uint32_t* spirv,
    size_t spirv_size,
    const PsbcCompileOptions* opts,
    PsbcShaderOutput* out
) {
    return psbc_compile_impl(spirv, spirv_size, NULL, NULL, 0, NULL,
                             opts, out);
}

PsbcResult psbc_compile_nir(
    const struct nir_shader* nir,
    const PsbcCompileOptions* opts,
    PsbcShaderOutput* out
) {
    return psbc_compile_impl(NULL, 0, nir, NULL, 0, NULL, opts, out);
}

PsbcResult psbc_compile_geometry_pipeline(
    const uint32_t* vertex_spirv,
    size_t vertex_spirv_size,
    const uint32_t* geometry_spirv,
    size_t geometry_spirv_size,
    const PsbcCompileOptions* opts,
    PsbcShaderOutput* out
) {
    return psbc_compile_impl(geometry_spirv, geometry_spirv_size, NULL,
                             vertex_spirv, vertex_spirv_size, NULL,
                             opts, out);
}

PsbcResult psbc_compile_nir_geometry_pipeline(
    const struct nir_shader* vertex_nir,
    const struct nir_shader* geometry_nir,
    const PsbcCompileOptions* opts,
    PsbcShaderOutput* out
) {
    return psbc_compile_impl(NULL, 0, geometry_nir, NULL, 0, vertex_nir,
                             opts, out);
}

void psbc_free_output(PsbcShaderOutput* out) {
    if (!out)
        return;
    if (out->data)
        free(out->data);
    if (out->machine_code)
        free(out->machine_code);
    memset(out, 0, sizeof(*out));
}

const char* psbc_result_string(PsbcResult result) {
    switch (result) {
    case PSBC_RESULT_OK:               return "success";
    case PSBC_RESULT_INVALID_SPIRV:    return "invalid SPIR-V bytecode";
    case PSBC_RESULT_UNSUPPORTED_STAGE: return "unsupported shader stage";
    case PSBC_RESULT_COMPILE_NIR:      return "SPIR-V to NIR compilation failed";
    case PSBC_RESULT_COMPILE_ACO:      return "ACO shader compilation failed";
    case PSBC_RESULT_OUT_OF_MEMORY:    return "out of memory";
    case PSBC_RESULT_INTERNAL_ERROR:   return "internal error";
    case PSBC_RESULT_INVALID_ARGUMENT: return "invalid or unsupported argument";
    default:                           return "unknown error";
    }
}

PsbcStage psbc_stage_from_name(const char* name) {
    if (!name)
        return PSBC_STAGE_NONE;
    if (!strcmp(name, "vertex"))      return PSBC_STAGE_VERTEX;
    if (!strcmp(name, "tess-ctrl"))   return PSBC_STAGE_TESS_CTRL;
    if (!strcmp(name, "tess-eval"))   return PSBC_STAGE_TESS_EVAL;
    if (!strcmp(name, "geometry"))    return PSBC_STAGE_GEOMETRY;
    if (!strcmp(name, "fragment"))    return PSBC_STAGE_FRAGMENT;
    if (!strcmp(name, "compute"))     return PSBC_STAGE_COMPUTE;
    if (!strcmp(name, "task"))        return PSBC_STAGE_TASK;
    if (!strcmp(name, "export"))      return PSBC_STAGE_EXPORT;
    if (!strcmp(name, "local"))       return PSBC_STAGE_LOCAL;
    return PSBC_STAGE_NONE;
}
