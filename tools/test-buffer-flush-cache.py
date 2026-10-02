#!/usr/bin/env python3
"""Exercise actual cache helper across ranges, collisions and write/drain cycles."""
from pathlib import Path
import subprocess
p=Path(__file__).resolve().parent.parent
s=(p/'patches/ps5-draw-batching/ps5_screen.c').read_text()
a=s.index('struct ps5_batch_flush_cache {');b=s.index('\nstatic bool\nps5_static_buffer_flush_cacheable(',a)
out=p/'build/draw-batching-driver';out.mkdir(parents=True,exist_ok=True)
c=out/'buffer-flush-cache-test.c'
c.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdalign.h>
#define PS5_GPU_PRESENT_BATCH 1
static unsigned calls;
static const void *last_data;
static size_t last_bytes;
enum ps5_flush_group { PS5_FLUSH_OTHER };
static void ps5_flush_gpu_data_group(const void *data,size_t bytes,enum ps5_flush_group group) {
 (void)group;
 calls++;last_data=data;last_bytes=bytes;
}
'''+s[a:b]+r'''
int main(void) {
 alignas(64) unsigned char backing[65536]={0};
 struct ps5_batch_flush_cache cache={0};
 ps5_flush_batch_backing(&cache,0,backing+62,4, PS5_FLUSH_OTHER);
 assert(calls==1 && last_data==backing && last_bytes==66);
 // Selected index spans flush both touched cache lines, not whole EBO.
 assert(last_bytes<sizeof(backing));
 ps5_flush_batch_backing(&cache,7,backing+62,4, PS5_FLUSH_OTHER);assert(calls==1);
 ps5_flush_batch_backing(&cache,7,backing+64,4, PS5_FLUSH_OTHER);assert(calls==2);
 ps5_flush_batch_backing(&cache,7,backing+64,8, PS5_FLUSH_OTHER);assert(calls==3);
 // A retained buffer write drains the batch, which zeroes its flush cache.
 backing[64]=42;memset(&cache,0,sizeof(cache));
 ps5_flush_batch_backing(&cache,7,backing+64,8, PS5_FLUSH_OTHER);assert(calls==4);
 ps5_flush_batch_backing(NULL,0,backing+64,8, PS5_FLUSH_OTHER);assert(calls==5);
 ps5_flush_batch_backing(NULL,0,backing+64,8, PS5_FLUSH_OTHER);assert(calls==6);
 // Hash collisions must cause extra flushes, never suppress a different range.
 unsigned offsets[256];for(unsigned i=0;i<256;i++) offsets[i]=UINT32_MAX;
 unsigned first=0,second=0;bool_found:
 for(unsigned off=0;off<sizeof(backing);off+=64) {
  uintptr_t address=(uintptr_t)(backing+off);
  unsigned bucket=((address>>8)^(address>>16)^(address>>24)^8)&255;
  if(offsets[bucket]!=UINT32_MAX){first=offsets[bucket];second=off;break;}
  offsets[bucket]=off;
 }
 assert(first!=second);memset(&cache,0,sizeof(cache));unsigned before=calls;
 ps5_flush_batch_backing(&cache,0,backing+first,8, PS5_FLUSH_OTHER);
 ps5_flush_batch_backing(&cache,0,backing+second,8, PS5_FLUSH_OTHER);
 ps5_flush_batch_backing(&cache,0,backing+first,8, PS5_FLUSH_OTHER);assert(calls==before+3);
 puts("Buffer cache: exact reuse, selected unaligned span coverage, changed ranges, post-write drain reset, null-cache fallback and collisions pass.");
}
'''.replace('bool_found:\n',''))
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(out/'buffer-flush-cache-test')],check=True)
subprocess.run([str(out/'buffer-flush-cache-test')],check=True)
