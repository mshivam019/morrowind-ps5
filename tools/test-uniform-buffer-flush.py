#!/usr/bin/env python3
"""Run actual static backing policy on ordinary and hazardous UBO lifecycles."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'patches/ps5-draw-batching/ps5_screen.c').read_text()
a=s.index('static bool\nps5_static_buffer_flush_cacheable')
b=s.index('static bool\nps5_stage_packed_depth_samples',a)
code=s[a:b]
a=s.index('static void\nps5_mark_gpu_write_role')
b=s.index('\nstruct ps5_transfer',a)
role=s[a:b]
# Assert integration still validates external ranges first and leaves copied
# bank-local uniforms on their unconditional flush path.
a=s.index('static bool\nps5_prepare_constant(')
b=s.index('static bool\nps5_tessellation_buffer_layout',a)
constant=s[a:b]
copied=constant[constant.index('if (state->copied)'):constant.index('descriptor[0] =')]
assert 'ps5_descriptor_flush_record(descriptor_flush, storage->data,' in copied
assert 'copied_offset, copied_offset + state->size, PS5_FLUSH_UNIFORM' in copied
assert 'state->size > buffer->size - state->offset' in copied
assert copied.index('state->size > buffer->size - state->offset') < copied.index('ps5_flush_static_buffer_backing(NULL, buffer,')
fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define PIPE_BUFFER 1
#define PIPE_BIND_VERTEX_BUFFER 1
#define PIPE_BIND_INDEX_BUFFER 2
#define PIPE_BIND_SHADER_BUFFER 4
#define PIPE_BIND_STREAM_OUTPUT 8
#define PIPE_BIND_SHADER_IMAGE 16
#define PIPE_BIND_SAMPLER_VIEW 32
#define PIPE_BIND_CONSTANT_BUFFER 64
#define PIPE_USAGE_DEFAULT 0
#define PIPE_USAGE_IMMUTABLE 1
#define PIPE_USAGE_DYNAMIC 2
#define PIPE_USAGE_STREAM 3
#define PS5_RESOURCE_FLAG_UNINITIALIZED 1
struct pipe_resource { unsigned target,bind,usage,flags; };
struct ps5_resource { struct pipe_resource base; bool exclusive_buffer_storage,cpu_pointer_escaped; unsigned cpu_write_maps; bool cpu_dirty; unsigned char *data; size_t size; };
struct ps5_batch_flush_cache { int unused; };
enum ps5_flush_group { PS5_FLUSH_UNIFORM,PS5_FLUSH_VERTEX };
static unsigned calls;
static const void *last;
static size_t span;
static void ps5_flush_batch_backing(struct ps5_batch_flush_cache *cache,unsigned slot,const void *data,size_t bytes,enum ps5_flush_group group) {
 (void)cache;(void)slot;(void)group;++calls;last=data;span=bytes;
}
static void ps5_mark_cpu_written(const struct pipe_resource *base) {
 if(base)((struct ps5_resource*)base)->cpu_dirty=true;
}
''' +code+role+r'''
static void uniform(struct ps5_resource *r,size_t offset,size_t bytes) {
 assert(offset<=r->size && bytes<=r->size-offset);
 ps5_flush_static_buffer_backing(NULL,r,r->data+offset,bytes,PS5_FLUSH_UNIFORM);
}
int main(void) {
 unsigned char memory[8192];
 struct ps5_resource r={{PIPE_BUFFER,PIPE_BIND_CONSTANT_BUFFER,PIPE_USAGE_DEFAULT,0},true,false,0,true,memory,sizeof(memory)};
 uniform(&r,256,64);
 assert(calls==1 && last==memory && span==sizeof(memory) && !r.cpu_dirty);
 for(unsigned i=0;i<600;++i)uniform(&r,(i%100)*64,32);
 assert(calls==1); // All future selected ranges were covered, not only upload0.
 ps5_mark_cpu_written(&r.base);uniform(&r,4096,256);
 assert(calls==2 && span==sizeof(memory) && !r.cpu_dirty);
 r.cpu_write_maps=1;ps5_mark_cpu_written(&r.base);
 uniform(&r,128,16);uniform(&r,256,16);
 assert(calls==4 && last==memory+256 && span==16 && r.cpu_dirty);
 r.cpu_write_maps=0;ps5_mark_cpu_written(&r.base);uniform(&r,128,16);
 assert(calls==5 && span==sizeof(memory) && !r.cpu_dirty);
 // Permanent escaping excludes raw pointer/persistent/coherent maps, and
 // actual later GPU write bindings, independently of original creationflags.
 ps5_mark_gpu_write_role(&r.base);uniform(&r,64,32);uniform(&r,128,32);
 assert(calls==7 && span==32 && r.cpu_dirty && r.cpu_pointer_escaped);
 r.cpu_pointer_escaped=false;
 unsigned hazards[]={PIPE_BIND_SHADER_BUFFER,PIPE_BIND_STREAM_OUTPUT,PIPE_BIND_SHADER_IMAGE,PIPE_BIND_SAMPLER_VIEW};
 for(unsigned i=0;i<4;++i) {
  r.base.bind=PIPE_BIND_CONSTANT_BUFFER|hazards[i];
  unsigned before=calls;uniform(&r,64,32);uniform(&r,128,32);
  assert(calls==before+2 && span==32 && r.cpu_dirty);
 }
 r.base.bind=PIPE_BIND_CONSTANT_BUFFER;
 for(unsigned usage=PIPE_USAGE_DYNAMIC;usage<=PIPE_USAGE_STREAM;++usage) {
  r.base.usage=usage;unsigned before=calls;uniform(&r,64,32);uniform(&r,128,32);
  assert(calls==before+2 && span==32 && r.cpu_dirty);
 }
 r.base.usage=PIPE_USAGE_IMMUTABLE;r.base.flags=PS5_RESOURCE_FLAG_UNINITIALIZED;
 uniform(&r,128,32);assert(span==32 && r.cpu_dirty);
 r.base.flags=0;r.exclusive_buffer_storage=false;
 uniform(&r,128,32);assert(span==32 && r.cpu_dirty);
 r.exclusive_buffer_storage=true;uniform(&r,128,32);assert(span==sizeof(memory) && !r.cpu_dirty);
 // Same storage later used as vertex input is already whollyclean; a write
 // before either role invalidates the shared epoch.
 r.base.bind=PIPE_BIND_VERTEX_BUFFER|PIPE_BIND_CONSTANT_BUFFER;
 unsigned before=calls;ps5_flush_static_buffer_backing(NULL,&r,memory+4096,128,PS5_FLUSH_VERTEX);
 assert(calls==before);
 ps5_mark_cpu_written(&r.base);uniform(&r,2048,64);assert(calls==before+1 && span==sizeof(memory));
 puts("PASS actual UBO flush policy: 601 static selections -> 1 whole flush; write epochs, maps, escaped/GPU roles, mutable/private/alias fallback, cross-role reuse; copied uniforms keep their owned range visibility.");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.c').write_text(fixture)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
