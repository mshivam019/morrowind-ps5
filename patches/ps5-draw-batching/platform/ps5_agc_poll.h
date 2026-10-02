#ifndef PS5_AGC_POLL_H
#define PS5_AGC_POLL_H
#include <stdint.h>
/* One initial spin window. Invalid or stalled clocks use the original sleep
 * loop, whose iteration bound remains enforced by the caller. */
struct ps5_agc_poll_state {
    int64_t start_ns, previous_ns;
    unsigned checks, stalled_checks, sleeps;
    int spin_done;
};
static inline int ps5_agc_poll_expired(const struct ps5_agc_poll_state *state, int64_t now)
{
    return state->start_ns > 0 && now >= state->start_ns &&
           now - state->start_ns >= INT64_C(2000000000);
}
/* 0: spin, 1: sleep, -1: deadline. Completion must still be tested by caller. */
static inline int ps5_agc_poll_step(struct ps5_agc_poll_state *state, int64_t now)
{
    if (ps5_agc_poll_expired(state, now))
        return -1;
    if (!state->spin_done) {
        const int valid_clock = state->start_ns > 0 && now >= state->start_ns &&
            (!state->previous_ns || now >= state->previous_ns);
        if (now == state->previous_ns)
            ++state->stalled_checks;
        else
            state->stalled_checks = 0;
        state->previous_ns = now;
        if (valid_clock &&
            now - state->start_ns < INT64_C(50000) &&
            ++state->checks <= 4096 && state->stalled_checks < 64)
            return 0;
        state->spin_done = 1;
    }
    ++state->sleeps;
    return 1;
}
#endif
