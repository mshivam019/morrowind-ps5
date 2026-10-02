#include <assert.h>
#include <stdio.h>
#include "compiler/nir/nir_builder.h"
#include "psbc_compile.h"
static nir_shader *shader(unsigned location, int fragment) {
 nir_builder b=nir_builder_init_simple_shader(fragment?MESA_SHADER_FRAGMENT:MESA_SHADER_VERTEX,
    psbc_get_nir_options(fragment?PSBC_STAGE_FRAGMENT:PSBC_STAGE_VERTEX),"legacy-varying");
 if (fragment) {
  nir_variable *input=nir_variable_create(b.shader,nir_var_shader_in,glsl_vec4_type(),"legacy");
  input->data.location=location; input->data.interpolation=INTERP_MODE_SMOOTH;
  nir_variable *color=nir_variable_create(b.shader,nir_var_shader_out,glsl_vec4_type(),"color");
  color->data.location=FRAG_RESULT_DATA0;
  nir_store_var(&b,color,nir_load_var(&b,input),15);
 } else {
  nir_variable *position=nir_variable_create(b.shader,nir_var_shader_out,glsl_vec4_type(),"pos");
  position->data.location=VARYING_SLOT_POS;
  nir_variable *clip=nir_variable_create(b.shader,nir_var_shader_out,glsl_vec4_type(),"clip_vertex");
  clip->data.location=VARYING_SLOT_CLIP_VERTEX;
  nir_variable *output=nir_variable_create(b.shader,nir_var_shader_out,glsl_vec4_type(),"legacy");
  output->data.location=location;
  nir_def *v=nir_u2f32(&b,nir_load_vertex_id_zero_base(&b));
  nir_store_var(&b,position,nir_vec4(&b,v,v,nir_imm_float(&b,0),nir_imm_float(&b,1)),15);
  nir_store_var(&b,clip,nir_vec4(&b,v,v,v,nir_imm_float(&b,1)),15);
  nir_store_var(&b,output,nir_vec4(&b,v,v,v,nir_imm_float(&b,1)),15);
 }
 nir_shader_gather_info(b.shader,nir_shader_get_entrypoint(b.shader));
 return b.shader;
}
int main(void) {
 unsigned locations[]={VARYING_SLOT_COL0,VARYING_SLOT_COL1,VARYING_SLOT_FOGC,VARYING_SLOT_TEX0,VARYING_SLOT_TEX7,VARYING_SLOT_BFC0,VARYING_SLOT_BFC1,VARYING_SLOT_VAR0,VARYING_SLOT_VAR31};
 psbc_init();
 for(unsigned i=0;i<sizeof(locations)/sizeof(locations[0]);i++) {
  unsigned location=locations[i]; PsbcShaderOutput outputs[2]={{0}};
  for(int fragment=0;fragment<2;fragment++) {
   nir_shader *nir=shader(location,fragment);
   PsbcCompileOptions opts={.target=PSBC_TARGET_PS5,.stage=fragment?PSBC_STAGE_FRAGMENT:PSBC_STAGE_VERTEX,.entrypoint="main",.optimise=true,.ngg=!fragment,.omit_implicit_primitive_id=true,.spi_shader_col_format=9};
   PsbcResult result=psbc_compile_nir(nir,&opts,&outputs[fragment]);
   printf("location=%u stage=%u result=%d code=%zu in=%u out=%u\n",location,fragment,result,outputs[fragment].machine_code_size,outputs[fragment].metadata.input_semantic_count,outputs[fragment].metadata.output_semantic_count);
   assert(result==PSBC_RESULT_OK && outputs[fragment].machine_code_size);
   ralloc_free(nir);
  }
  assert(outputs[0].metadata.output_semantic_count==1 && outputs[1].metadata.input_semantic_count==1);
  assert((outputs[0].metadata.output_semantics[0]&255)==(outputs[1].metadata.input_semantics[0]&255));
  psbc_free_output(&outputs[0]);psbc_free_output(&outputs[1]);
 }
 nir_shader *active=shader(VARYING_SLOT_COL0,0);
 nir_builder b=nir_builder_at(nir_after_impl(nir_shader_get_entrypoint(active)));
 nir_variable *distance=nir_variable_create(active,nir_var_shader_out,glsl_float_type(),"clip_distance");
 distance->data.location=VARYING_SLOT_CLIP_DIST0;
 nir_store_var(&b,distance,nir_imm_float(&b,1),1);
 active->info.clip_distance_array_size=1;
 nir_shader_gather_info(active,nir_shader_get_entrypoint(active));
 PsbcCompileOptions opts={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_VERTEX,.entrypoint="main",.optimise=true,.ngg=true,.omit_implicit_primitive_id=true};
 PsbcShaderOutput output={0};
 assert(psbc_compile_nir(active,&opts,&output)==PSBC_RESULT_OK);
 assert(output.metadata.clip_distance_mask==1);
 printf("Active clip distance preserved mask=%u\n",output.metadata.clip_distance_mask);
 psbc_free_output(&output);ralloc_free(active);
 psbc_shutdown();
}
