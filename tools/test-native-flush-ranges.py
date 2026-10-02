#!/usr/bin/env python3
"""Test actual native cache-line traversal, intercepting only assembly effects."""
from pathlib import Path
import subprocess
from cache_flush_test_fixture import intercepted_header
root=Path(__file__).resolve().parent.parent
source=(root/'patches/ps5-draw-batching/platform/ps5_agc_native_runtime.c').read_text()
a=source.index('static void flush_gpu_data(const void *address, size_t bytes)\n{')
b=source.index('\n}',a)+2
body=source[a:b]
prefix='''#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
unsigned detect_calls; bool test_support;
static uintptr_t flushed[1024];
static size_t count, fences;
static void record(uintptr_t at, bool optimized) { (void)optimized; assert(count<1024); flushed[count++]=at; }
static void fence(void) { ++fences; }
'''
main='''
int main(void) {
 for(size_t offset=0;offset<64;++offset) {
  for(size_t length=0;length<=4096;++length) {
   uintptr_t begin=0x10000+offset;
   count=fences=0;
   flush_gpu_data((const void*)begin,length);
   size_t expected=length ? ((begin+length-1)/64-begin/64+1):0;
   assert(count==expected&&fences==1);
   for(size_t i=0;i<count;++i) assert(flushed[i]==(begin&~(uintptr_t)63)+64*i);
  }
 }
 count=fences=0;flush_gpu_data(NULL,0);assert(!count&&fences==1);
 puts("PASS native flush: all64 offsets, lengths0..4096, unaligned tails, zero size, fence retained");
}
'''
out=root/'build/native-flush-tests';out.mkdir(parents=True,exist_ok=True)
(out/'flush.c').write_text(prefix+intercepted_header(record='record',fence='fence')+body+main)
subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(out/'flush.c'),'-o',str(out/'flush')],check=True)
subprocess.run([str(out/'flush')],check=True)
