#!/usr/bin/env python3
"""Compile the actual full eligibility function with minimal driver fixtures."""
import os
from pathlib import Path
import subprocess
p=Path(__file__).resolve().parent.parent
s=(p/'patches/ps5-draw-batching/ps5_screen.c').read_text()
a=s.index('static bool\nps5_multidraw_eligible');b=s.index('\nstatic bool\nps5_batch_copy_descriptors',a)
out=p/'build/draw-batching-driver';c=out/'depth-only-batching-test.c'
gl=Path(str(Path.home() / 'ps5sdk/ps5-opengl-030/ps5-opengl'));mesa=gl/'third_party/mesa-26.2.0'
c.write_text(r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include "pipe/p_state.h"
#define PS5_MAX_TEXTURE_UNITS 16
#define PS5_CPU_PROFILE_ADD(field,value) atomic_fetch_add(&ps5_cpu_profile.field,(value))
struct ps5_resource {struct pipe_resource base;void *data;size_t size;
 size_t depth_staging_size,render_staging_size;void *render_pool_owner;};
struct ps5_shader {int placeholder;};
struct ps5_context {
 struct {struct pipe_surface cbufs[8],zsbuf;unsigned nr_cbufs;} framebuffer;
 const struct pipe_depth_stencil_alpha_state *depth_stencil_alpha;
 bool deferred_clear,framebuffer_valid;struct {bool running;} *blitter;
 struct ps5_shader *vs,*fs,*gs;
 unsigned stream_output_target_count,vertex_buffer_count;
 void *render_condition_query,*active_occlusion_query;
 struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
 struct pipe_sampler_view *sampler_views[2][PS5_MAX_TEXTURE_UNITS];
};
static struct {atomic_uint_fast64_t rejected_mips,rejected_format,rejected_staging;} ps5_cpu_profile;
static bool ps5_shader_uses_storage(const struct ps5_shader *s){(void)s;return false;}
static unsigned ps5_shader_texture_count(const struct ps5_shader *s){(void)s;return 0;}
static bool ps5_any_primitive_query(const struct ps5_context *c){(void)c;return false;}
static bool ps5_texture_used(const struct ps5_context *c,const struct ps5_shader *s,const void *m,unsigned u){(void)c;(void)s;(void)m;(void)u;return false;}
static bool ps5_linear_sampled_layout(const struct pipe_resource *r){(void)r;return true;}
static bool ps5_batch_mip_view_eligible(const struct pipe_resource *r,const struct pipe_sampler_view *v,bool l,bool a){(void)r;(void)v;(void)l;(void)a;return true;}
static void ps5_batch_texture_rejection(const struct ps5_resource *r,const struct pipe_sampler_view *v,bool a){(void)r;(void)v;(void)a;}
'''+s[a:b]+r'''
int main(void) {
 struct ps5_resource depth={.base={.target=PIPE_TEXTURE_2D,.format=PIPE_FORMAT_Z32_FLOAT_S8X24_UINT}};
 struct pipe_depth_stencil_alpha_state dsa={0};struct ps5_shader shader={0};
 struct ps5_context c={.framebuffer_valid=true,.vs=&shader,.fs=&shader,.depth_stencil_alpha=&dsa};
 c.framebuffer.zsbuf.texture=&depth.base;c.framebuffer.zsbuf.format=depth.base.format;
 struct pipe_draw_info info={.mode=MESA_PRIM_TRIANGLES,.instance_count=1,.index_size=2,.index.resource=&depth.base};
 struct pipe_draw_start_count_bias draw={.count=9};
 assert(ps5_multidraw_eligible(&c,&info,NULL,&draw,1));
 info.index_size=4;assert(ps5_multidraw_eligible(&c,&info,NULL,&draw,1));
 info.index_size=0;assert(ps5_multidraw_eligible(&c,&info,NULL,&draw,1));
 dsa.stencil[0].enabled=true;assert(!ps5_multidraw_eligible(&c,&info,NULL,&draw,1));dsa.stencil[0].enabled=false;
 dsa.stencil[1].enabled=true;assert(!ps5_multidraw_eligible(&c,&info,NULL,&draw,1));dsa.stencil[1].enabled=false;
 depth.depth_staging_size=64;assert(!ps5_multidraw_eligible(&c,&info,NULL,&draw,1));depth.depth_staging_size=0;
 depth.base.nr_samples=4;assert(!ps5_multidraw_eligible(&c,&info,NULL,&draw,1));depth.base.nr_samples=0;
 c.framebuffer.zsbuf.level=1;assert(!ps5_multidraw_eligible(&c,&info,NULL,&draw,1));c.framebuffer.zsbuf.level=0;
 info.mode=MESA_PRIM_TRIANGLE_STRIP;assert(!ps5_multidraw_eligible(&c,&info,NULL,&draw,1));info.mode=MESA_PRIM_TRIANGLES;
 c.framebuffer.zsbuf.texture=NULL;assert(!ps5_multidraw_eligible(&c,&info,NULL,&draw,1));
 puts("Actual batching eligibility: depth-only triangles accept 16/32-bit/nonindexed; stencil/staging/MSAA/mip/strip/missingdepth remain rejected.");
}
''')
subprocess.run(['cc','-std=c11','-D_GNU_SOURCE','-DHAVE_ENDIAN_H','-I'+str(gl/'build/mesa-ps5-probe/src'),'-I'+str(mesa/'src/gallium/include'),'-I'+str(mesa/'src'),'-I'+str(mesa/'include'),str(c),'-o',str(out/'depth-only-batching-test')],check=True)
subprocess.run([str(out/'depth-only-batching-test')],check=True)
