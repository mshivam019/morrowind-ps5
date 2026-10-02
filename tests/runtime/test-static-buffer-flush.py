#!/usr/bin/env python3
"""Exercise production candidate buffer clean policy across changing draw ranges."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'patches/ps5-draw-batching/ps5_screen.c').read_text()
start=s.index('static bool\nps5_static_buffer_flush_cacheable')
end=s.index('static bool\nps5_stage_packed_depth_samples',start)
code=s[start:end]
role_start=s.index("static void\nps5_mark_gpu_write_role")
role_end=s.index("\nstruct ps5_transfer",role_start)
role=s[role_start:role_end]
h='''#include <cassert>
#include <cstddef>
#define PIPE_BUFFER 1
#define PIPE_BIND_CONSTANT_BUFFER 64
#define PIPE_BIND_VERTEX_BUFFER 1
#define PIPE_BIND_INDEX_BUFFER 2
#define PIPE_BIND_SHADER_BUFFER 4
#define PIPE_BIND_STREAM_OUTPUT 8
#define PIPE_BIND_SHADER_IMAGE 16
#define PIPE_BIND_SAMPLER_VIEW 32
#define PIPE_USAGE_DEFAULT 0
#define PIPE_USAGE_IMMUTABLE 1
#define PIPE_USAGE_DYNAMIC 2
#define PIPE_USAGE_STREAM 3
#define PS5_RESOURCE_FLAG_UNINITIALIZED 1
struct pipe_resource { unsigned target=PIPE_BUFFER,bind=PIPE_BIND_VERTEX_BUFFER|PIPE_BIND_INDEX_BUFFER,usage=PIPE_USAGE_DEFAULT,flags=0; };
struct ps5_resource { pipe_resource base; bool exclusive_buffer_storage=true,cpu_pointer_escaped=false; unsigned cpu_write_maps=0; bool cpu_dirty=true; char data[4096]; size_t size=4096; };
struct ps5_batch_flush_cache {};
enum ps5_flush_group { PS5_FLUSH_VERTEX, PS5_FLUSH_INDEX };
unsigned calls=0;const void*last_data=nullptr;size_t last_size=0;
void ps5_flush_batch_backing(ps5_batch_flush_cache*,unsigned,const void*data,size_t size,ps5_flush_group){++calls;last_data=data;last_size=size;}
void ps5_mark_cpu_written(const pipe_resource *base){if(base)((ps5_resource*)base)->cpu_dirty=true;}
'''+code+role+'''
int main(){
 ps5_resource r;ps5_batch_flush_cache cache;
 auto draw=[&](size_t off,size_t bytes){ps5_flush_static_buffer_backing(&cache,&r,r.data+off,bytes,PS5_FLUSH_VERTEX);};
 draw(64,32);assert(calls==1&&last_data==r.data&&last_size==r.size&&!r.cpu_dirty);
 for(unsigned i=0;i<600;++i){draw((i%32)*64,32);}assert(calls==1);
 // Subdata and write-unmap invalidate full-backing epoch.
 r.cpu_dirty=true;draw(2048,64);assert(calls==2&&last_size==4096);
 r.cpu_write_maps=1;r.cpu_dirty=true;draw(32,16);assert(calls==3&&last_size==16&&r.cpu_dirty);
 r.cpu_write_maps=0;r.cpu_dirty=true;draw(32,16);assert(calls==4&&last_size==4096);
 r.cpu_pointer_escaped=true;draw(64,32);draw(128,16);assert(calls==6&&last_size==16);
 r.cpu_pointer_escaped=false;
 // GL roles can change without original creation bind flags changing.
 r.cpu_dirty=false;ps5_mark_gpu_write_role(&r.base);
 assert(r.cpu_dirty&&r.cpu_pointer_escaped);
 draw(64,32);assert(last_size==32);
 ps5_mark_gpu_write_role(nullptr);
 r.cpu_pointer_escaped=false;
 for(unsigned usage:{PIPE_USAGE_STREAM,PIPE_USAGE_DYNAMIC}){r.base.usage=usage;draw(64,32);assert(last_size==32);}
 r.base.usage=PIPE_USAGE_IMMUTABLE;r.cpu_dirty=true;draw(64,32);assert(last_size==4096);
 for(unsigned bind:{PIPE_BIND_SHADER_BUFFER,PIPE_BIND_STREAM_OUTPUT,PIPE_BIND_SHADER_IMAGE,PIPE_BIND_SAMPLER_VIEW}){r.base.bind=PIPE_BIND_VERTEX_BUFFER|bind;draw(64,32);assert(last_size==32);}
 r.base.bind=PIPE_BIND_INDEX_BUFFER;r.base.flags=PS5_RESOURCE_FLAG_UNINITIALIZED;draw(64,32);assert(last_size==32);
 r.base.flags=0;r.exclusive_buffer_storage=false;draw(64,32);assert(last_size==32);
}
'''
h=h.replace('#include <cstddef>','#include <cstddef>\n#include <initializer_list>')
with tempfile.TemporaryDirectory(prefix='static-buffer-policy-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(h)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS whole-backing upload/repeat-ranges/subdata/map/escape/laterGPUrole/mutable/private/alias')
