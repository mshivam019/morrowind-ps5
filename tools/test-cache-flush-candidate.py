#!/usr/bin/env python3
"""Exhaustively exercise actual CPUID-gated visibility traversal and ordering."""
import os
from pathlib import Path
import subprocess,os,shlex
from cache_flush_test_fixture import ROOT,intercepted_header
out=ROOT/'build/cache-flush-candidate-tests';out.mkdir(parents=True,exist_ok=True)
prefix='''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
static bool test_support=TEST_SUPPORT;
static unsigned detect_calls;
static uintptr_t recorded[1024];
static bool choices[1024];
static size_t count,fences;
static void record_line(uintptr_t at,bool optimized) {
 assert(count<1024);recorded[count]=at;choices[count++]=optimized;
 assert(fences==(optimized?1u:0u));
}
static void record_fence(void) { ++fences; }
'''
main='''
int main(void) {
 assert(ps5_cache_flush_capable(7,8u<<8,1u<<19,1u<<23));
 assert(!ps5_cache_flush_capable(6,8u<<8,1u<<19,1u<<23));
 assert(!ps5_cache_flush_capable(7,4u<<8,1u<<19,1u<<23));
 assert(!ps5_cache_flush_capable(7,8u<<8,0,1u<<23));
 assert(!ps5_cache_flush_capable(7,8u<<8,1u<<19,0));
 for(unsigned offset=0;offset<64;++offset) for(unsigned length=0;length<=4096;++length) {
  count=fences=0;uintptr_t begin=0x10000+offset;
  ps5_cache_flush_range((void*)begin,length);
  size_t expected=length?((begin+length-1)/64-begin/64+1):0;
  bool optimized=PS5_CLFLUSHOPT&&TEST_SUPPORT&&length;
  assert(count==expected&&fences==(optimized?2u:1u));
  for(size_t i=0;i<count;++i) assert(recorded[i]==(begin&~(uintptr_t)63)+64*i&&choices[i]==optimized);
 }
 assert(detect_calls==(PS5_CLFLUSHOPT?1u:0u));
 count=fences=0;ps5_cache_flush_range(NULL,0);assert(!count&&fences==1);
 puts("PASS shared cacheflush: capability/fallback, all64 offsets lengths0..4096, pre/post fences, one-time detection");
}
'''
for enabled,support in ((0,0),(0,1),(1,0),(1,1)):
 path=out/f'host-{enabled}-{support}';path.with_suffix('.c').write_text(prefix+intercepted_header()+main)
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-variable','-DPS5_LIGHT_DIAGNOSTICS=0',f'-DPS5_CLFLUSHOPT={enabled}',f'-DTEST_SUPPORT={support}',str(path.with_suffix('.c')),'-o',str(path)],check=True)
 subprocess.run([str(path)],check=True)
sdk=Path(str(Path.home() / 'ps5sdk'));env=dict(os.environ,PS5_PAYLOAD_SDK=str(sdk/'native-app-boilerplate/.deps/native/ps5-payload-sdk'))
proof=out/'target.c';proof.write_text('#include "ps5_cache_flush.h"\nvoid proof(const void*p,size_t n){ps5_cache_flush_range(p,n);}\n')
for enabled in (0,1):
 obj=out/f'target-{enabled}.o'
 subprocess.run([str(sdk/'native-app-boilerplate/tooling/prospero-clang18'),'-std=c11','-O2','-Wall','-Wextra','-Werror',f'-DPS5_CLFLUSHOPT={enabled}','-DPS5_LIGHT_DIAGNOSTICS=1','-I'+str(ROOT/'patches/ps5-draw-batching/platform'),'-c',str(proof),'-o',str(obj)],env=env,check=True)
 text=subprocess.check_output(['llvm-objdump','-d',str(obj)],text=True)
 assert '\tclflush\t' in text and '\tmfence' in text
 assert ('\tclflushopt\t' in text)==bool(enabled)
 assert ('\tcpuid' in text)==bool(enabled)
print('PASS private target objects: gated CPUID+CLFLUSHOPT and legacy CLFLUSH/MFENCE fallback')

# Compile both real integration translation units privately; never mutate an archive.
gl=sdk/'ps5-opengl-030/ps5-opengl'
lines=(sdk/'extracted/ps5-opengl-sdk-0.3.0/runtime-config.txt').read_text().splitlines()
for enabled in (0,1):
 for name,selected,source in (
   ('screen',lines[1:],ROOT/'patches/ps5-draw-batching/ps5_screen.c'),
   ('native',(lines[1],lines[4]),ROOT/'patches/ps5-draw-batching/platform/ps5_agc_runtime_backend.c')):
  flags=[x.replace('/home/runner/work/ps5-opengl/ps5-opengl',str(gl)) for l in selected for x in shlex.split(l)]
  extra=['-Dmain=ps5_agc_gate2_run','-DAGC_TRIANGLE_SUBMIT=1','-DAGC_RUNTIME_PACKAGES=1'] if name=='native' else []
  subprocess.run([str(sdk/'native-app-boilerplate/tooling/prospero-clang18'),*flags,'-UPS5_DRAW_PROFILE','-Wno-error=unused-function','-Wno-error=unused-variable','-I'+str(gl/'third_party/opengnm-psbc/libpsbc'),'-I'+str(gl/'src/platform'),*extra,f'-DPS5_CLFLUSHOPT={enabled}','-DPS5_LIGHT_DIAGNOSTICS=1','-c',str(source),'-o',str(out/f'{name}-{enabled}.o')],env=env,check=True)
print('PASS private native and screen integration objects OFF/ON')
