#include <assert.h>
#include <stdio.h>
#include "ps5_agc_poll.h"
static unsigned fake_wait(int64_t completion_ns, int valid_clock, int *timed_out) {
 struct ps5_agc_poll_state s={.start_ns=valid_clock?1000:0};
 int64_t now=s.start_ns;
 *timed_out=0;
 for(unsigned i=0;i<100000;i++) {
  if(ps5_agc_poll_expired(&s,now)){*timed_out=1;return s.sleeps;}
  if(valid_clock && now>=completion_ns)return s.sleeps;
  int action=ps5_agc_poll_step(&s,now);
  if(action<0){*timed_out=1;return s.sleeps;}
  if(valid_clock)now+=action?125000:1000;
 }
 *timed_out=1;return s.sleeps;
}
int main(void) {
 struct ps5_agc_poll_state s={.start_ns=1000};
 for(int i=0;i<50;i++)assert(ps5_agc_poll_step(&s,1000+i*1000)==0);
 assert(ps5_agc_poll_step(&s,51000)==1);
 assert(ps5_agc_poll_step(&s,52000)==1); /* Never start another spin window. */
 assert(ps5_agc_poll_step(&s,2000001000LL)==-1);
 s=(struct ps5_agc_poll_state){.start_ns=1000,.previous_ns=1000};
 for(int i=0;i<63;i++)assert(ps5_agc_poll_step(&s,1000)==0);
 assert(ps5_agc_poll_step(&s,1000)==1); /* Stalled-clock guard. */
 s=(struct ps5_agc_poll_state){.start_ns=1000};
 assert(ps5_agc_poll_step(&s,0)==1);
 s=(struct ps5_agc_poll_state){.start_ns=1000,.previous_ns=2000};
 assert(ps5_agc_poll_step(&s,1500)==1);
 int timeout;
 assert(fake_wait(21000,1,&timeout)==0 && !timeout);
 assert(fake_wait(101000,1,&timeout)==1 && !timeout);
 assert(fake_wait(3000000000LL,1,&timeout)>0 && timeout);
 assert(fake_wait(0,0,&timeout)==100000 && timeout);
 puts("Polling smoke checks: fast completion/no sleep, slow completion, strict deadline, single spin, stalled/zero/backward clocks and original iteration bound passed.");
}
