#ifndef PS5_AGC_PROFILE_H
#define PS5_AGC_PROFILE_H
#include <stdint.h>
/* Same submitting thread only. Nanosecond totals reset on runtime shutdown.
 * Consumers report differences between snapshots, never per-draw logs. */
struct ps5_agc_profile_snapshot {
    uint64_t prepare_ns[5];
    uint64_t submit_ns[9];
    uint64_t present_ns[3];
    uint64_t batch_ns[4];
    uint64_t prepare_calls, prepare_failures;
    uint64_t submit_calls, submit_failures, submit_sleeps;
    uint64_t present_calls, present_failures;
    uint64_t batch_calls, batch_failures, batch_sleeps;
};
/* Returns 1 when profiling is enabled, 0 otherwise. Does not reset counters. */
int ps5_agc_gate2_profile_snapshot(struct ps5_agc_profile_snapshot *snapshot);
void ps5_agc_gate2_default_cache_snapshot(uint64_t *hits, uint64_t *misses);
#endif
