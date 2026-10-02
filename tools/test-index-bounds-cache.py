#!/usr/bin/env python3
"""Exercise actual static EBO cache and write invalidation against scan results."""
from pathlib import Path
import subprocess
p=Path(__file__).resolve().parent.parent
s=(p/'patches/ps5-draw-batching/ps5_screen.c').read_text()
cache=s[s.index('struct ps5_index_bounds_cache {'):s.index('\nstruct ps5_resource {')]
a=s.index('static bool\nps5_static_buffer_flush_cacheable');policy=s[a:s.index('\nstatic void\nps5_flush_static_buffer_backing',a)]
a=s.index('static inline void\nps5_mark_cpu_written');write=s[a:s.index('\n/* Creation bind flags',a)]
a=s.index('static bool\nps5_index_bounds(');code=s[a:s.index('\n#ifndef PS5_RUNTIME_QUIET',a)]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define MIN2(a,b) ((a)<(b)?(a):(b))
#define MAX2(a,b) ((a)>(b)?(a):(b))
#define PIPE_BUFFER 1
#define PIPE_BIND_CONSTANT_BUFFER 64
#define PIPE_BIND_VERTEX_BUFFER 1
#define PIPE_BIND_INDEX_BUFFER 2
#define PIPE_BIND_SHADER_BUFFER 4
#define PIPE_BIND_STREAM_OUTPUT 8
#define PIPE_BIND_SHADER_IMAGE 16
#define PIPE_BIND_SAMPLER_VIEW 32
#define PIPE_BIND_DISPLAY_TARGET 64
#define PIPE_USAGE_DEFAULT 0
#define PIPE_USAGE_IMMUTABLE 1
#define PIPE_USAGE_DYNAMIC 2
#define PIPE_USAGE_STREAM 3
#define PS5_RESOURCE_FLAG_UNINITIALIZED 1
'''+cache+r'''
struct pipe_resource {unsigned target,bind,usage,flags;};
struct ps5_resource {struct pipe_resource base;bool exclusive_buffer_storage,cpu_pointer_escaped,cpu_dirty;unsigned cpu_write_maps;struct ps5_index_bounds_cache index_bounds[4];};
static bool ps5_scanout_cpu_dirty;
static uint64_t reads;
static uint32_t ps5_index_value(const void *p,unsigned width,unsigned i){reads++;return width==2?((const uint16_t*)p)[i]:((const uint32_t*)p)[i];}
'''
main=r'''
int main(void){
 struct ps5_resource r={.base={PIPE_BUFFER,PIPE_BIND_INDEX_BUFFER,PIPE_USAGE_DEFAULT,0},.exclusive_buffer_storage=true};
 uint32_t indices[16384];for(unsigned i=0;i<16384;i++)indices[i]=i%1000;
 struct ps5_index_bounds_cache b;
 for(unsigned i=0;i<600;i++)assert(ps5_index_bounds(&r,indices,0,4,16384,0,&b));
 assert(reads==16384&&b.min_index==0&&b.max_effective==999);
 printf("600 repeated 16k-index draws: cached reads=%llu, uncached reads=9830400 (600x reduction).\n",(unsigned long long)reads);
 // Tracked writes invalidate exact matching keys before the next scan.
 indices[0]=3000;ps5_mark_cpu_written(&r.base);
 assert(ps5_index_bounds(&r,indices,0,4,16384,0,&b)&&b.max_effective==3000&&reads==32768);
 // Width, range and bias all matter; preserve negative bias validation.
 uint16_t small[]={3,9,5};
 assert(ps5_index_bounds(&r,small,64,2,3,-3,&b)&&b.min_effective==0&&b.max_effective==6);
 assert(!ps5_index_bounds(&r,small,64,2,3,-4,&b));
 assert(ps5_index_bounds(&r,small+1,66,2,2,0,&b)&&b.min_index==5);
 uint32_t edge[]={UINT32_MAX};
 assert(!ps5_index_bounds(&r,edge,128,4,1,0,&b));
 assert(ps5_index_bounds(&r,edge,128,4,1,1,&b)&&b.max_effective==0); // preserve original wrapping semantics
 // Mutable, active-mapped and escaped buffers must rescan even identical keys.
 r.base.usage=PIPE_USAGE_DYNAMIC;uint64_t before=reads;
 for(unsigned i=0;i<2;i++)assert(ps5_index_bounds(&r,small,64,2,3,0,&b));assert(reads==before+6);
 r.base.usage=PIPE_USAGE_DEFAULT;r.cpu_write_maps=1;before=reads;
 for(unsigned i=0;i<2;i++)assert(ps5_index_bounds(&r,small,64,2,3,0,&b));assert(reads==before+6);
 r.cpu_write_maps=0;r.cpu_pointer_escaped=true;before=reads;
 for(unsigned i=0;i<2;i++)assert(ps5_index_bounds(&r,small,64,2,3,0,&b));assert(reads==before+6);
 // Collision eviction never reuses another range's bounds.
 r.cpu_pointer_escaped=false;ps5_mark_cpu_written(&r.base);
 assert(ps5_index_bounds(&r,small,0,2,3,0,&b));
 assert(ps5_index_bounds(&r,indices,16,2,3,0,&b));
 assert(ps5_index_bounds(&r,small,0,2,3,0,&b)&&b.min_index==3&&b.max_index==9);
 puts("PASS tracked writes, exact range/width/bias, negativebias/overflow, mutable/map/escape fallback.");
}
'''
out=p/'build/draw-batching-driver';c=out/'index-bounds-cache-test.c';c.write_text(pre+policy+write+code+main)
subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',str(c),'-o',str(out/'index-bounds-cache-test')],check=True)
subprocess.run([str(out/'index-bounds-cache-test')],check=True)
