#!/usr/bin/env python3
"""Compile/test actual drain/capacity functions and private PS5 screen objects."""
from pathlib import Path
import os,shlex,subprocess,tempfile
p=Path(__file__).resolve().parents[1]
s=(p/'patches/ps5-draw-batching/ps5_screen.c').read_text()
def function(name):
 a=s.index('\n'+name+'(');a=s.rfind('static void',0,a);b=s.index('\n{',a)+2;depth=1;end=b+1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[a:end]
code='''#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "ps5_batch_break.h"
struct batch {void *owner;unsigned count;uint64_t token;} ps5_deferred,ps5_pending;
static struct ps5_batch_break_stats ps5_break_stats;
static unsigned submits,releases;
#define PS5_CPU_PROFILE_CLOCK() 0
#define PS5_CPU_PROFILE_ADD(field,value) ((void)0)
#define PS5_RECORD_BREAK(reason,count) ps5_batch_break_record(&ps5_break_stats,reason,count)
static int ps5_agc_gate2_batch_end(void){++submits;return 0;}
static int ps5_agc_gate2_batch_end_async(uint64_t *token){++submits;*token=123;return 0;}
static void ps5_deferred_release(struct batch *b){++releases;b->owner=0;b->count=0;}
static void ps5_pending_retire_locked(void){if(ps5_pending.owner){++ps5_break_stats.pending_retirements;ps5_deferred_release(&ps5_pending);}}
'''+function('ps5_draw_batch_flush_locked')+'\n'+function('ps5_draw_batch_capacity_locked')+'''
int main(void){
 ps5_draw_batch_flush_locked(PS5_BREAK_EXTERNAL);assert(!submits);
 ps5_deferred=(struct batch){(void*)1,4,0};ps5_draw_batch_flush_locked(PS5_BREAK_RESOURCE_HAZARD);
 assert(submits==1&&ps5_break_stats.batches[PS5_BREAK_RESOURCE_HAZARD]==1&&ps5_break_stats.draws[PS5_BREAK_RESOURCE_HAZARD]==4);
 ps5_deferred=(struct batch){(void*)1,128,0};ps5_draw_batch_capacity_locked();
 assert(submits==2&&ps5_break_stats.batches[PS5_BREAK_CAPACITY]==1&&ps5_break_stats.draws[PS5_BREAK_CAPACITY]==128);
 ps5_draw_batch_flush_locked(PS5_BREAK_PRESENT);
 assert(submits==2&&ps5_break_stats.batches[PS5_BREAK_PRESENT]==0);
#if PS5_ASYNC_BATCH
 assert(ps5_break_stats.pending_retirements==1&&releases==2);
#else
 assert(ps5_break_stats.pending_retirements==0&&releases==2);
#endif
 ps5_batch_break_record(&ps5_break_stats,PS5_BREAK_QUERY,0);
 ps5_batch_break_record(&ps5_break_stats,PS5_BREAK_REASON_COUNT,5);
 assert(ps5_break_stats.batches[PS5_BREAK_QUERY]==0);
}
'''
with tempfile.TemporaryDirectory() as d:
 src=Path(d)/'test.c';exe=Path(d)/'test';src.write_text(code)
 for asynchronous in (0,1):
  subprocess.run(['cc','-std=c11','-O2','-DPS5_ASYNC_BATCH='+str(asynchronous),'-I'+str(p/'patches/ps5-draw-batching/platform'),str(src),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
print('Actual drain/capacity tests pass: empty skips, reasons/draws counted, async submission not double-counted.')
sdk=Path(os.environ.get('PS5SDK_ROOT',str(Path.home() / 'ps5sdk')));gl=sdk/'ps5-opengl-030/ps5-opengl'
lines=(sdk/'extracted/ps5-opengl-sdk-0.3.0/runtime-config.txt').read_text().splitlines()
flags=[f.replace('/home/runner/work/ps5-opengl/ps5-opengl',str(gl)) for line in lines[1:] for f in shlex.split(line)]
flags=['-O2' if f=='-Os' else f for f in flags]
out=p/'build/batch-break-test';out.mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env['PS5_PAYLOAD_SDK']=str(sdk/'native-app-boilerplate/.deps/native/ps5-payload-sdk')
for light,asynchronous in ((0,0),(1,0),(1,1)):
 target=out/f'screen-light{light}-async{asynchronous}.o'
 subprocess.run([str(sdk/'native-app-boilerplate/tooling/prospero-clang18'),*flags,'-I'+str(gl/'src/platform'),'-UPS5_DRAW_PROFILE','-DPS5_RUNTIME_QUIET=1','-DPS5_LIGHT_DIAGNOSTICS='+str(light),'-DPS5_ASYNC_BATCH='+str(asynchronous),'-Wno-error=unused-function','-Wno-error=unused-variable','-c',str(p/'patches/ps5-draw-batching/ps5_screen.c'),'-o',str(target)],env=env,check=True)
 raw=target.read_bytes();assert (b'[ps5-batch-breaks]' in raw)==bool(light)
 relocs=subprocess.check_output(['llvm-objdump','-r',str(target)],text=True)
 assert relocs.count('os_time_get_nano')==3,'Light counters must not add clocks'
print('PS5 off/on/async compilation passes; off object omits receipts and each object keeps only3query clocks.')
