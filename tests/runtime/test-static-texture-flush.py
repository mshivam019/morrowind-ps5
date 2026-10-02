#!/usr/bin/env python3
"""Run the candidate's actual clean-bit eligibility and sampler flush block."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'patches/ps5-draw-batching/ps5_screen.c').read_text()
start=s.index('static bool\nps5_static_texture_flush_cacheable')
end=s.index('\nstatic inline void\nps5_mark_cpu_written',start)
policy=s[start:end]
start=s.index('         const bool cacheable = ps5_static_texture_flush_cacheable(texture);')
end=s.index('\n      }\n   }\n   if (texture_count',start)
flush=s[start:end]
harness='''#include <cassert>
#include <cstddef>
#define PIPE_TEXTURE_2D 2
#define PIPE_BUFFER 1
#define PIPE_BIND_SAMPLER_VIEW 1
#define PIPE_BIND_RENDER_TARGET 2
#define PIPE_BIND_DEPTH_STENCIL 4
#define PIPE_BIND_DISPLAY_TARGET 8
#define PIPE_BIND_SHADER_IMAGE 16
struct pipe_resource { unsigned target=PIPE_TEXTURE_2D,bind=PIPE_BIND_SAMPLER_VIEW; };
struct ps5_resource { pipe_resource base; size_t render_staging_size=0,depth_staging_size=0; void* render_pool_owner=nullptr; bool cpu_pointer_escaped=false; unsigned cpu_write_maps=0; bool cpu_dirty=true; void* data=nullptr; size_t size=4096; };
enum { PS5_FLUSH_TEXTURE };
unsigned calls=0;
void ps5_flush_batch_backing(void*,unsigned,void*,size_t,int){++calls;}
'''+policy+'''
void sample(ps5_resource* texture) {
 unsigned slot=1,unit=0; bool merged_geometry=false; void* flush_cache=nullptr;
'''+flush+'''
}
int main(){
 ps5_resource r; sample(&r); assert(calls==1&&!r.cpu_dirty);
 for(unsigned i=0;i<600;++i) { sample(&r); } assert(calls==1);
 // Ordinary write-map: keep flushing while active; unmap invalidates again.
 r.cpu_write_maps=1;r.cpu_dirty=true;sample(&r);sample(&r);assert(calls==3&&r.cpu_dirty);
 r.cpu_write_maps=0;r.cpu_dirty=true;sample(&r);sample(&r);assert(calls==4);
 // CPU clear, blit/copyback and mip writes all invalidate before next sample.
 for(unsigned write=0;write<4;++write){r.cpu_dirty=true;sample(&r);sample(&r);}assert(calls==8);
 // RT capability and allocated unused color staging are not CPU writes.
 r.base.bind|=PIPE_BIND_RENDER_TARGET;r.render_staging_size=4096;
 sample(&r);sample(&r);assert(calls==8);
 // Actual GPU copyback/staging CPU writes invalidate before later sampling.
 r.cpu_dirty=true;sample(&r);sample(&r);assert(calls==9);
 // Depth/display/image-store remain conservative even with a clean bit.
 r.base.bind|=PIPE_BIND_SHADER_IMAGE;sample(&r);sample(&r);assert(calls==11);
 r.base.bind=PIPE_BIND_SAMPLER_VIEW|PIPE_BIND_RENDER_TARGET;
 // Raw-pointer escape and persistent/coherent maps are permanent exclusions.
 r.cpu_pointer_escaped=true;r.cpu_dirty=false;sample(&r);sample(&r);assert(calls==13);
 r.cpu_pointer_escaped=false;r.depth_staging_size=4096;sample(&r);sample(&r);assert(calls==15);
}
'''
with tempfile.TemporaryDirectory(prefix='static-texture-policy-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(harness)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS upload/repeat/write-map/unmap/CPUwrites/RT-capability/copyback/image-store/escape/depth-staging')
