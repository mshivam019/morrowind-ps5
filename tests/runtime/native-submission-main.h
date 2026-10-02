static void queue(agc_api_t *api,int unknown) {
 uint32_t *p=aligned_alloc(256,256);memset(p,0,256);
 uintptr_t marker=(uintptr_t)(p+32);
 p[0]=unknown?0xc0043f00:0xc0042700;p[1]=6;p[2]=0x10000;
 p[3]=0;p[4]=6;p[5]=0;
 p[6]=0xc0064900;p[7]=0x528;p[8]=0x20000000;
 p[9]=(uint32_t)marker;p[10]=(uint32_t)(marker>>32);p[11]=123;
 agc_submit_description_t d={p,14,0,{0}};
 assert(!runtime_batch_queue(api,&d,p,1,256,p+32,123));
}
int main(void){
 agc_api_t api={submit,suspend_point};uint64_t token;
 /* Poison inaccessible tail with an invalid owner. A live-count bug would
  * submit or free it; preserving it proves clearing is bounded as intended. */
 runtime_batch_entries[255].memory=(void *)(uintptr_t)0x1234;
 runtime_pending_batch.entries[255].memory=(void *)(uintptr_t)0x5678;
 assert(!ps5_agc_gate2_batch_begin());queue(&api,0);queue(&api,0);
 assert(!ps5_agc_gate2_batch_end_async(&token)&&token&&submitted==1&&released==0);
 assert(ps5_agc_gate2_present(0)==-1);
 assert(ps5_agc_gate2_shutdown_present()==-1);
 assert(runtime_batch_entries[255].memory==(void *)(uintptr_t)0x1234);
 assert(runtime_pending_batch.entries[255].memory==(void *)(uintptr_t)0x5678);
 assert(ps5_agc_gate2_batch_retire(token+1)==-1&&released==0);
 assert(!ps5_agc_gate2_batch_begin());queue(&api,0);
 uint64_t bad=999;assert(ps5_agc_gate2_batch_end_async(&bad)==-1);
 assert(!ps5_agc_gate2_batch_retire(token)&&released==3);
 assert(!runtime_pending_batch.count&&!runtime_pending_batch.attempted);
 assert(runtime_pending_batch.entries[255].memory==(void *)(uintptr_t)0x5678);
 assert(ps5_agc_gate2_batch_retire(token)==-1);
 assert(!ps5_agc_gate2_batch_end()&&submitted==2&&released==4);
 assert(!ps5_agc_gate2_present(0)&&!ps5_agc_gate2_shutdown_present());
 assert(!ps5_agc_gate2_batch_begin());queue(&api,1);queue(&api,0);
 /* Unknown packet fallback submitted separately; fake GPU walker still preserves words. */
 assert(!ps5_agc_gate2_batch_end()&&submitted==4&&released==6);
 assert(runtime_coalesced_batches==1&&runtime_fallback_batches==2);
 assert(!ps5_agc_gate2_batch_begin());queue(&api,0);queue(&api,0);
 submit_result=1;token=999;
 assert(ps5_agc_gate2_batch_end_async(&token)==-1&&!token);
 assert(released==6&&fatal==1&&runtime_batch_faulted);
 assert(ps5_agc_gate2_batch_begin()==-1);
 puts("native submission: one-vs-N submits, pending ownership, next recording, strict tokens passed");
}
