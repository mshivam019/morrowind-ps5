#!/usr/bin/env python3
"""Exercise actual screen timestamp with recorded and pending GPU work."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'patches/ps5-draw-batching/ps5_screen.c').read_text()
def function(name):
 start=source.index(name+'(')
 begin=source.index('{',start)
 end=begin+1; depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
body=function('ps5_get_timestamp')
assert 'ps5_draw_batch_drain' not in body
for name in ['ps5_begin_query','ps5_end_query']:
 assert 'ps5_draw_batch_drain(PS5_BREAK_QUERY);' in function(name)
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
struct pipe_screen { int unused; };
static uint64_t clock_ns=123456789;
static unsigned active_draws=128,pending_draws=128,retained_refs=256;
static unsigned clock_reads,drains;
static uint64_t os_time_get_nano(void) { ++clock_reads; return clock_ns++; }
static void ps5_draw_batch_drain(unsigned reason) {
 (void)reason;++drains;active_draws=pending_draws=retained_refs=0;
}
#define PS5_BREAK_QUERY 0
static uint64_t
'''+body+r'''
int main(void) {
 struct pipe_screen screen={0};
 for(unsigned i=0;i<600;++i) {
  assert(ps5_get_timestamp(&screen)==123456789+i);
  assert(active_draws==128 && pending_draws==128 && retained_refs==256);
 }
 assert(clock_reads==600 && drains==0);
 // A real ordered query still crosses the explicit completion boundary.
 ps5_draw_batch_drain(PS5_BREAK_QUERY);
 assert(drains==1 && !active_draws && !pending_draws && !retained_refs);
 puts("PASS actual timestamp: 600 immediate clock samples preserve active/pending ownership; query begin/end drains retained.");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.c').write_text(program)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
