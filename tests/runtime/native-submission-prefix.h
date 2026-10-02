#include "ps5_agc_coalesce.h"
#include "ps5_agc_poll.h"
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>
static unsigned runtime_present_count;
#include <stdlib.h>
#include <string.h>
#define PS5_MULTIDRAW_BATCH 1
#define PS5_MULTIDRAW_BATCH_CAPACITY 256
#define PS5_NATIVE_SUBMIT_COALESCE 1
#define PS5_ASYNC_BATCH 1
struct agc_submit_description {void *words;uint32_t word_count;uint8_t flag;uint8_t pad[3];};
typedef struct agc_submit_description agc_submit_description_t;
typedef struct {int (*submit)(const agc_submit_description_t*);int (*suspend_point)(void);} agc_api_t;
static unsigned submitted,released,fatal;
static int submit_result;
static int submit(const agc_submit_description_t *d) {
 ++submitted;
 if(submit_result) return submit_result;
 for(uint32_t i=0;i<d->word_count;) {
  uint32_t n=(( ((uint32_t*)d->words)[i] >>16)&0x3fff)+2;
  uint32_t *w=(uint32_t*)d->words+i;
  if(((w[0]>>8)&255)==0x49&&((w[2]>>29)&7)==1) {
   uintptr_t address=(uint64_t)w[3]|((uint64_t)w[4]<<32);
   *(uint32_t*)address=w[5]; /* Simulated GPU completion, not runtime shortcut. */
  } i+=n;
 } return 0;
}
static int suspend_point(void){return 0;}
static int64_t os_time_get_nano(void){static int64_t t;return ++t;}
static void flush_gpu_data(const void *p,size_t n){(void)p;(void)n;}
static int sceKernelUsleep(uint32_t x){(void)x;return 0;}
static int64_t sceKernelGetDirectMemorySize(void){return 1;}
static void runtime_require_retirement(int complete){if(!complete) ++fatal;}
static int runtime_work_acquire(int64_t limit,size_t n,uint8_t **p,int64_t *direct,size_t *bytes){(void)limit;*p=aligned_alloc(256,n);*direct=1;*bytes=n;return !*p;}
static int runtime_work_release(void *p,int64_t direct,size_t n){(void)direct;(void)n;++released;free(p);return 0;}
