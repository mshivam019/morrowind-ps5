#!/usr/bin/env python3
"""Exercise actual EGL pre-swap screen boundary before ordinary VideoOut flip."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'patches/ps5-draw-batching/ps5_screen.c').read_text()
a=s.index('void\nps5_context_queue_present(');b=s.index('\nstatic void\nps5_deferred_release',a)
body=s[a:b]
assert 'ps5_agc_gate2_batch_present' not in body
assert 'ps5_draw_batch_flush_locked(PS5_BREAK_PRESENT);' in body
# The real retirement/release implementation is covered by the existing
# async ownership fixture. Here exercise caller ordering and first-present
# readiness independently of any native GPU-flip append API.
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
struct pipe_context { int unused; };
struct ps5_context { struct pipe_context base; };
static struct { struct ps5_context *owner; unsigned count; } ps5_deferred;
static int ps5_deferred_mutex;
static bool locked,video_registered;
static unsigned pending_refs,active_refs,drains,empty_receipts,queued_receipts,flips;
#define PS5_BREAK_PRESENT 9
static void ps5_cpu_profile_present(void) {}
static void simple_mtx_lock(int *mutex) { (void)mutex;assert(!locked);locked=true; }
static void simple_mtx_unlock(int *mutex) { (void)mutex;assert(locked);locked=false; }
static void ps5_batch_break_present_locked(bool queued) {
 assert(locked);if(queued)++queued_receipts;else++empty_receipts;
}
static void ps5_draw_batch_flush_locked(unsigned reason) {
 assert(locked && reason==PS5_BREAK_PRESENT);++drains;
 pending_refs=active_refs=0;ps5_deferred.count=0;ps5_deferred.owner=NULL;
}
'''+body+r'''
static void egl_ordinary_flip(void) {
 assert(!locked && !pending_refs && !active_refs && !ps5_deferred.count);
 // First swap can register VideoOut here AFTER the owned screen drain.
 video_registered=true;++flips;
}
int main(void) {
 struct ps5_context context={0},other={0};
 assert(!video_registered);
 ps5_deferred.owner=&context;ps5_deferred.count=7;active_refs=21;
 ps5_context_queue_present(&context.base,0);
 assert(drains==1 && queued_receipts==1 && !video_registered);
 egl_ordinary_flip();assert(video_registered && flips==1);
 // Async pending plus newly recorded work must BOTH retire before flip.
 ps5_deferred.owner=&context;ps5_deferred.count=3;active_refs=9;pending_refs=384;
 ps5_context_queue_present(&context.base,1);
 assert(drains==2 && queued_receipts==2);egl_ordinary_flip();
 // Pending-only and process-global ownership by another context still drain.
 pending_refs=128;ps5_context_queue_present(&context.base,0);egl_ordinary_flip();
 ps5_deferred.owner=&other;ps5_deferred.count=5;active_refs=15;
 ps5_context_queue_present(&context.base,1);egl_ordinary_flip();
 assert(drains==4 && empty_receipts==2 && flips==4);
 // Already retired/empty frame preserves the same legal flip path.
 ps5_context_queue_present(&context.base,0);egl_ordinary_flip();
 assert(drains==5 && empty_receipts==3 && flips==5);
 puts("PASS actual present boundary: first-unregistered swap, active+pending work, pending-only, cross-context, empty frames retire before ordinary flip.");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.c').write_text(program)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
