#ifndef PS5_AGC_ASYNC_H
#define PS5_AGC_ASYNC_H
#include <stdint.h>
/* Submission thread, under the global submit lock. Ownership transfers only
 * after successful real submission; native work and screen resources remain
 * pinned until the matching blocking retirement succeeds. One pending token. */
int ps5_agc_gate2_batch_end_async(uint64_t *token);
int ps5_agc_gate2_batch_retire(uint64_t token);
#endif
