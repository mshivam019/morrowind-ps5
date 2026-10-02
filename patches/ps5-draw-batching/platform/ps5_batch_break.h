#ifndef PS5_BATCH_BREAK_H
#define PS5_BATCH_BREAK_H
#include <stdint.h>
#ifndef PS5_LIGHT_DIAGNOSTICS
#define PS5_LIGHT_DIAGNOSTICS 0
#endif
/* Categories describe why recording work submits, not why a draw is rejected.
 * Pending retirement is separate: an async capacity submission counts once. */
enum ps5_batch_break_reason {
   PS5_BREAK_CAPACITY, PS5_BREAK_OWNER_CHANGE, PS5_BREAK_INELIGIBLE,
   PS5_BREAK_RESOURCE_HAZARD, PS5_BREAK_QUERY, PS5_BREAK_EXPLICIT,
   PS5_BREAK_EXTERNAL, PS5_BREAK_ALLOCATION_FAILURE, PS5_BREAK_DRAW_FAILURE,
   PS5_BREAK_PRESENT, PS5_BREAK_REASON_COUNT
};
struct ps5_batch_break_stats {
   uint64_t batches[PS5_BREAK_REASON_COUNT], draws[PS5_BREAK_REASON_COUNT];
   uint64_t pending_retirements, presents, queued_present_boundaries, empty_present_boundaries;
};
static inline void
ps5_batch_break_record(struct ps5_batch_break_stats *stats,
                       enum ps5_batch_break_reason reason, unsigned draws)
{
   if (!stats || (unsigned)reason >= PS5_BREAK_REASON_COUNT || !draws)
      return;
   ++stats->batches[reason];
   stats->draws[reason] += draws;
}
#endif
