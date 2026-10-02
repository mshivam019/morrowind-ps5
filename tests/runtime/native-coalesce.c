#include "ps5_agc_coalesce.h"
#include <assert.h>
#include <stdio.h>
struct owner { uint32_t words[64]; uint32_t marker; };
static void build(struct owner *o) {
   uintptr_t marker=(uintptr_t)&o->marker;
   /* Exact color-to-texture barrier emitted by native runtime backend. */
   const uint32_t barrier[]={0xc0064900,0x0070f52d,0x00010000,0,0,0,0,0};
   memcpy(o->words,barrier,sizeof(barrier));
   const uint32_t draw[]={0xc0012d00,3,2};
   memcpy(o->words+8,draw,sizeof(draw));
   const uint32_t completion[]={0xc0064900,0x00000528,0x20000000,
     (uint32_t)marker,(uint32_t)(marker>>32),123,0,0};
   memcpy(o->words+11,completion,sizeof(completion));
}
int main(void) {
 _Alignas(256) struct owner a={0};struct owner b={0}; build(&a); build(&b);
 struct ps5_agc_coalesce_stream s[2]={
  {a.words,19,&a,sizeof(a),&a.marker,123},
  {b.words,19,&b,sizeof(b),&b.marker,123}};
 _Alignas(256) uint32_t destination[128]; uint32_t n;
 memset(destination,0xa5,sizeof(destination));
 assert(ps5_agc_coalesce(s,2,destination,128,&n)&&n==38);
 assert(!memcmp(destination,a.words,76)&&!memcmp(destination+19,b.words,76));
 assert(!ps5_agc_coalesce(s,2,a.words,128,&n)); /* Output may not alias an owner. */
 assert(a.marker==0&&b.marker==0); /* CPU copy must never spoof retirement. */
 b.words[0]=0xc0013f00; /* Unknown indirect control flow: whole batch rejected. */
 memset(destination,0xa5,sizeof(destination));
 assert(!ps5_agc_coalesce(s,2,destination,128,&n)&&!n);
 assert(destination[0]==0xa5a5a5a5); build(&b);
 s[1].count=18; assert(!ps5_agc_coalesce(s,2,destination,128,&n)); s[1].count=19;
 assert(!ps5_agc_coalesce(s,2,destination,37,&n));
 s[1].owner_bytes=16; assert(!ps5_agc_coalesce(s,2,destination,128,&n));s[1].owner_bytes=sizeof(b);
 b.words[13]=0; assert(!ps5_agc_coalesce(s,2,destination,128,&n));build(&b);
 s[1].expected=999;assert(!ps5_agc_coalesce(s,2,destination,128,&n));s[1].expected=123;
 assert(!ps5_agc_coalesce(s,257,destination,128,&n));
 assert(!ps5_agc_coalesce_contains((void*)(UINTPTR_MAX-3),8,(void*)UINTPTR_MAX,1));
 /* Mesa RADV/RadeonSI emits PKT3(DRAW_INDEX_2,4): header plus
  * maxsize, index address low/high, index count and initiator =6 words. */
 memmove(a.words+14,a.words+11,8*sizeof(uint32_t));
 const uint32_t indexed[]={0xc0042700,6,0x10000,0,6,0};
 memcpy(a.words+8,indexed,sizeof(indexed));s[0].count=22;
 assert(ps5_agc_coalesce(s,2,destination,128,&n)&&n==41);
 assert(!memcmp(destination+8,indexed,sizeof(indexed)));
 memmove(a.words+15,a.words+14,8*sizeof(uint32_t));
 a.words[8]=0xc0052700;a.words[14]=0;s[0].count=23;
 assert(!ps5_agc_coalesce(s,2,destination,128,&n)); /* Well-framed7 is invalid. */
 build(&a);s[0].count=19;
 uintptr_t table=(uintptr_t)(a.words+40);
 memmove(a.words+16,a.words+11,8*sizeof(uint32_t));
 a.words[11]=0xc0039f00;a.words[12]=(uint32_t)table;
 a.words[13]=(uint32_t)(table>>32);a.words[14]=0;a.words[15]=2;
 s[0].count=24;
 assert(ps5_agc_coalesce(s,2,destination,128,&n)&&n==43);
 assert(destination[12]==(uint32_t)table&&destination[13]==(uint32_t)(table>>32));
 a.words[15]=1000;assert(!ps5_agc_coalesce(s,2,destination,128,&n));
 puts("native coalesce: packet order, completion, atomic fallback and ownership passed");
}
