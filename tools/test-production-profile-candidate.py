#!/usr/bin/env python3
"""Exercise actual flush body/profile macros and disabled snapshot getter."""
from pathlib import Path
import subprocess,tempfile
from cache_flush_test_fixture import intercepted_header
p=Path(__file__).resolve().parent.parent
s=(p/'patches/ps5-draw-batching/ps5_screen.c').read_text()
a=s.index('/* Detailed instrumentation');b=s.index('\n#ifdef PS5_DRAW_PROFILE\nstruct ps5_cpu_profile',a)
macros=s[a:b]
a=s.index('static void\nps5_flush_gpu_data_group(');b=s.index('\n/* Per-draw CPU writes',a)
flush=s[a:b]
pre='''#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <assert.h>
#include <stdbool.h>
unsigned detect_calls; bool test_support;
static unsigned clocks,cleans,fences;
static void record_line(uintptr_t at,bool optimized){(void)at;(void)optimized;++cleans;}
static void record_fence(void){++fences;}
static int64_t os_time_get_nano(void){return ++clocks;}
enum ps5_flush_group { PS5_FLUSH_OTHER,PS5_FLUSH_GROUP_COUNT };
struct { _Atomic uint64_t flush_ns,group_ns[1],group_bytes[1],group_calls[1],flush_bytes,flush_calls;} ps5_cpu_profile;
'''
main='''int main(void){_Alignas(64) uint8_t data[192];ps5_flush_gpu_data_group(data,sizeof data,PS5_FLUSH_OTHER);assert(cleans==3&&fences==1);
#ifdef PS5_DRAW_PROFILE
 assert(clocks==2&&ps5_cpu_profile.flush_calls==1&&ps5_cpu_profile.flush_bytes==192);
#else
 assert(clocks==0&&ps5_cpu_profile.flush_calls==0&&ps5_cpu_profile.flush_bytes==0);
#endif
}
'''
native=(p/'patches/ps5-draw-batching/platform/ps5_agc_native_runtime.c').read_text()
a=native.index('int ps5_agc_gate2_profile_snapshot(');b=native.index('\nstatic int64_t runtime_next_render_marker',a)
getter=native[a:b]
header=(p/'patches/ps5-production-profile/platform/ps5_agc_profile.h').read_text()
gettest='#include <string.h>\n#include <assert.h>\n'+header+getter+'''\nint main(void){struct ps5_agc_profile_snapshot s;memset(&s,255,sizeof s);assert(!ps5_agc_gate2_profile_snapshot(&s));for(unsigned i=0;i<sizeof s;i++)assert(!((unsigned char*)&s)[i]);assert(!ps5_agc_gate2_profile_snapshot(0));}'''
with tempfile.TemporaryDirectory() as d:
 for name,content,flags in [('flush-off',pre+intercepted_header()+macros+flush+main,[]),('flush-on',pre+intercepted_header()+macros+flush+main,['-DPS5_DRAW_PROFILE=1']),('snapshot-off',gettest,[])]:
  src=Path(d)/(name+'.c');exe=Path(d)/name;src.write_text(content)
  subprocess.run(['cc','-std=c11','-O2',*flags,str(src),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('Actual flush: identical cleans/fence, zero profiling clocks/updates when disabled; snapshot zero-filled.')
# Object relocation check enforces the three functional timestamp/query clocks.
out=subprocess.check_output(['llvm-objdump','-r',str(p/'build/production-profile-candidate/ps5_screen.o')],text=True)
assert out.count('os_time_get_nano')==3
assert 'ps5_agc_gate2_profile_snapshot' not in out
print('Production screen: exactly three query-clock references, no native profile calls.')
