#!/usr/bin/env python3
"""Exercise actual screen ownership/drain functions with deterministic fences."""
from pathlib import Path
import subprocess
p=Path(__file__).resolve().parent.parent
s=(p/'patches/ps5-draw-batching/ps5_screen.c').read_text()
a=s.index('static void\nps5_deferred_release');b=s.index('\nstatic void\nps5_outer_batch_rejection',a)
functions=s[a:b]
a=s.index('struct ps5_deferred_batch {');b=s.index('\nvoid\nps5_context_queue_present',a)
state=s[a:b]
# Verify external teardown/readback locks still reach the tested full drain.
for name in ['ps5_context_destroy','ps5_screen_destroy','ps5_context_last_draw_status']:
 a=s.index('\n'+name+'(');assert 'ps5_draw_batch_drain(PS5_BREAK_EXPLICIT);' in s[a:a+450]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include "ps5_batch_break.h"
#define PS5_ASYNC_BATCH 1
#define PS5_MULTIDRAW_BATCH_CAPACITY 2
#define PS5_BATCH_RESOURCE_COUNT 4
#define PIPE_BUFFER 1
#define PIPE_TEXTURE_2D 2
#define PIPE_BIND_DISPLAY_TARGET 64
#define PS5_RENDER_ARENA_OFFSET 128
#define PS5_CPU_PROFILE_CLOCK() 0
#define PS5_CPU_PROFILE_ADD(field,value) ((void)(value))
#define MIN2(a,b) ((a)<(b)?(a):(b))
struct pipe_resource {unsigned target,bind,refs;};
struct ps5_resource {struct pipe_resource base;unsigned deferred_uses;bool exclusive_buffer_storage;void *data,*stencil_data;size_t allocation_size,stencil_allocation_size;};
struct ps5_context {int id;};
struct ps5_batch_flush_cache {int unused;};
static int ps5_deferred_mutex;
static void simple_mtx_lock(int *p){(void)p;}
static void simple_mtx_unlock(int *p){(void)p;}
static unsigned releases;
static void pipe_resource_reference(struct pipe_resource **p,struct pipe_resource *next){if(*p){assert((*p)->refs>1);(*p)->refs--;releases++;}*p=next;}
static void ps5_deferred_use(struct pipe_resource *p,bool retain){if(p){struct ps5_resource *r=(struct ps5_resource*)p;if(retain)r->deferred_uses++;else{assert(r->deferred_uses);r->deferred_uses--;}}}
static uint64_t fence,next_fence;static bool complete,retire_failure,submit_failure;static unsigned retire_calls;
static int ps5_agc_gate2_batch_end_async(uint64_t *token){assert(!fence);if(submit_failure)return -1;*token=fence=++next_fence;complete=false;return 0;}
static int ps5_agc_gate2_batch_retire(uint64_t token){assert(token==fence);retire_calls++;if(retire_failure)return -1;complete=true;fence=0;return 0;}
static int ps5_agc_gate2_batch_end(void){assert(!fence);return 0;}
static jmp_buf fatal;static void ownership_fatal(int code){(void)code;longjmp(fatal,1);}
#define _Exit(code) ownership_fatal(code)
'''
main=r'''
static struct ps5_context context={1};
static unsigned char memory[1024];
static struct ps5_resource descriptor[2],vertex;
static void enqueue(unsigned bank){
 assert(!ps5_deferred.owner);ps5_deferred.owner=&context;ps5_deferred.count=1;ps5_deferred.bank=bank;
 struct ps5_resource *d=&descriptor[bank];d->base.refs++;d->deferred_uses++;
 vertex.base.refs++;vertex.deferred_uses++;
 ps5_deferred.slots[0].storage[0]=&d->base;
 ps5_deferred.slots[0].retained[0]=&vertex.base;ps5_deferred.slots[0].retained_count=1;
}
int main(void){
 for(unsigned i=0;i<2;i++){descriptor[i].base=(struct pipe_resource){PIPE_BUFFER,0,1};descriptor[i].data=memory+i*128;descriptor[i].allocation_size=128;}
 vertex.base=(struct pipe_resource){PIPE_BUFFER,0,1};vertex.data=memory+512;vertex.allocation_size=128;vertex.exclusive_buffer_storage=true;
 enqueue(0);ps5_draw_batch_capacity_locked();
 assert(fence&&ps5_pending.owner&&ps5_pending.bank==0&&!ps5_deferred.owner);
 assert(descriptor[0].base.refs==2&&vertex.base.refs==2&&!releases&&!complete);
 // CPU can encode only the opposite bank while the GPU owns bank zero.
 enqueue(ps5_pending.bank^1);assert(ps5_deferred.bank==1&&descriptor[0].base.refs==2);
 struct ps5_resource upload=vertex;upload.data=memory+768;upload.deferred_uses=0;
 uint64_t older=fence;unsigned waits=retire_calls;
 ps5_draw_batch_drain_buffer(&upload.base);
 assert(fence==older&&retire_calls==waits&&ps5_pending.owner&&ps5_deferred.owner);
 ps5_draw_batch_capacity_locked();
 assert(retire_calls==1&&ps5_pending.bank==1&&descriptor[0].base.refs==1);
 assert(descriptor[1].base.refs==2&&vertex.base.refs==2);
 // A CPU buffer write drains pending ownership before it may mutate data.
 ps5_draw_batch_drain_buffer(&vertex.base);
 assert(!fence&&!ps5_pending.owner&&vertex.base.refs==1&&descriptor[1].base.refs==1);
 // Conservative alias map must also wait, even if exclusive-storage is false.
 struct ps5_resource alias=vertex;alias.exclusive_buffer_storage=false;
 enqueue(0);ps5_draw_batch_capacity_locked();ps5_draw_batch_drain_buffer(&alias.base);
 assert(!fence&&!ps5_pending.owner&&vertex.base.refs==1);
 // Readback, present and destruction use this same all-ownership drain.
 for(unsigned boundary=0;boundary<3;boundary++){enqueue(0);ps5_draw_batch_capacity_locked();enqueue(1);ps5_draw_batch_flush_locked(PS5_BREAK_EXPLICIT);assert(!fence&&!ps5_pending.owner&&!ps5_deferred.owner&&descriptor[0].base.refs==1&&descriptor[1].base.refs==1);}
 // Failed retirement pins refs and banks; no release can run on timeout.
 enqueue(0);ps5_draw_batch_capacity_locked();unsigned before=releases;retire_failure=true;
 if(!setjmp(fatal)){ps5_draw_batch_flush_locked(PS5_BREAK_EXPLICIT);assert(false);}
 assert(releases==before&&descriptor[0].base.refs==2&&ps5_pending.owner&&fence);
 retire_failure=false;ps5_draw_batch_flush_locked(PS5_BREAK_EXPLICIT);
 // Failed submission must not discard active batch resources either.
 enqueue(0);before=releases;submit_failure=true;
 if(!setjmp(fatal)){ps5_draw_batch_capacity_locked();assert(false);}
 assert(releases==before&&ps5_deferred.owner&&descriptor[0].base.refs==2);
 submit_failure=false;ps5_draw_batch_flush_locked(PS5_BREAK_EXPLICIT);
 assert(vertex.deferred_uses==0&&descriptor[0].deferred_uses==0&&descriptor[1].deferred_uses==0);
 puts("PASS actual screen ownership: two banks, real-token contract, pending refs, unrelated upload overlap, referenced CPU write/alias drains, readback/present/destruction drains, timeout and submit fault pinning.");
}
'''
out=p/'build/async-screen-candidate';out.mkdir(parents=True,exist_ok=True);c=out/'ownership-test.c';c.write_text(pre+state+functions+main)
subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(p/'patches/ps5-draw-batching/platform'),str(c),'-o',str(out/'ownership-test')],check=True)
subprocess.run([str(out/'ownership-test')],check=True)
