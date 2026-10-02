#!/usr/bin/env python3
"""Compare actual cached and canonical target-state output, including mutations."""
import os
from pathlib import Path
import subprocess, tempfile
p=Path(__file__).resolve().parent.parent
original=(Path(os.environ.get('PS5_OPENGL_ROOT', Path.home() / 'ps5sdk/ps5-opengl-030/ps5-opengl')) / 'src/platform/ps5_agc_native_runtime.c').read_text()
candidate=(p/'patches/ps5-draw-batching/platform/ps5_agc_native_runtime.c').read_text()
def extract(s,start):
    a=s.index(start); b=s.index('\n#if defined(AGC_DEPTH_TEST_VARIANT)',a)
    return s[a:b]
base=extract(original,'static int append_target_state(').replace('append_target_state','uncached')
cache=extract(candidate,'/* The submit lock serializes this cache.')
pre='''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define PS5_DRAW_PROFILE 1
static unsigned width=1920,height=1080;
#define DISPLAY_WIDTH width
#define DISPLAY_HEIGHT height
typedef struct { uint16_t offset,padding; uint32_t value; } agc_register_t;
static uint32_t float_bits(float f) { uint32_t v; memcpy(&v,&f,4);return v; }
'''
main='''
static void compare(void *defaults, uintptr_t target) {
 agc_register_t a[64]={{0}},b[64]={{0}};uint32_t na=0,nb=0;
 int ra=uncached(a,&na,defaults,(void*)target);
 int rb=append_target_state(b,&nb,defaults,(void*)target);
 assert(ra==rb); if(!ra){assert(na==nb);assert(!memcmp(a,b,na*sizeof(*a)));}
}
int main(void){
 uint16_t offsets[16]={0x318,0x31b,0x31c,0x31d,0x31e,0x31f,0x321,0x323,0x324,0x325,0x390,0x398,0x3a0,0x3a8,0x3b0,0x3b8};
 agc_register_t rows[512],other[512];agc_register_t *blocks[1]={rows};
 union {uint64_t align;uint8_t data[64];} defaults={0};
 *(agc_register_t ***)defaults.data=blocks;*(uint32_t*)(defaults.data+0x20)=512;
 for(unsigned i=0;i<512;i++) rows[i]=(agc_register_t){(uint16_t)(0x800+i),0,i*12345u};
 for(unsigned i=0;i<16;i++)rows[i*23+9].offset=offsets[i];
 for(unsigned i=0;i<1000;i++) {width=640+i%17;height=480+i%13;compare(defaults.data,0x10000000000ull+i*4096);}
 /* Values may change at the same pointer/count: every draw must see them. */
 for(unsigned i=0;i<512;i++)rows[i].value^=0xabcdef01u;
 compare(defaults.data,0x12345000000ull);
 /* Offset reordering invalidates cached locations. */
 agc_register_t t=rows[9];rows[9]=rows[32];rows[32]=t;
 compare(defaults.data,0x54321000000ull);
 memcpy(other,rows,sizeof rows);blocks[0]=other;
 compare(defaults.data,0x23456000000ull);
 *(uint32_t*)(defaults.data+0x20)=400;compare(defaults.data,0x34567000000ull);
 other[9].offset=0xffff;compare(defaults.data,0x34567000000ull);
 uint64_t hits,misses;ps5_agc_gate2_default_cache_snapshot(&hits,&misses);
 assert(hits>=1000 && misses>=4);
 printf("target command bytes equivalent: hits=%llu misses=%llu\\n",(unsigned long long)hits,(unsigned long long)misses);
}
'''
with tempfile.TemporaryDirectory() as d:
    src=Path(d)/'test.c';exe=Path(d)/'test';src.write_text(pre+base+cache+main)
    for flags in ([],['-DAGC_TEXTURE_VIEWPORT_HALF','-DAGC_TEXTURE_SCISSOR_HALF']):
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra',*flags,str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
