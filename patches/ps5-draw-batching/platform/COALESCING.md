Native submission candidates (offline validation)

PS5_NATIVE_SUBMIT_COALESCE and PS5_ASYNC_BATCH are independently gated numeric
macros, default 0. Production keeps separate submissions and blocking retirement.
The coalescer accepts only bounded, straight-line packet streams (<=256 streams,
<=65536 dwords) with a verified 32-bit completion write after the final draw.
Unknown opcodes, indirect execution, predicates, malformed packets, invalid table
ownership, excess capacity or allocation failure fall back to original submits.
All qualification finishes before output writes. Concatenation copies original
packet words in order, preserving every barrier and absolute indirect reference.
Original owners and joined storage remain alive until every completion marker
matches. Submit failure or unconfirmed retirement faults the queue and pins work.

Async uses one separate pending snapshot and a monotonically increasing token;
recording the next batch does not overwrite pending owners. Retirement requires
the exact token, blocks using the existing deadline/poll policy, and releases only
verified work. Synchronous end drains a pending batch before current work. Screen
ownership/bank guards are defined separately in ps5_screen.c.

Validation: source tools/env.sh; python3 tools/test-native-coalesce.py
Host fixtures compile actual native helpers, checking one vs N submit counts,
original/combined allocation retention, recording during pending work, wrong and
stale tokens, submit-failure pinning, exact packet order, truncation, overflow,
unknown-control-flow fallback, output alias rejection and original indirect tables.
One packet fixture uses the exact native backend color-release barrier words.
Private target objects cover both gates OFF, both ON, independently enabled,
and profiling ON. No installed archive is replaced by these checks.

Hardware qualification remains necessary. Complete firmware-AGC-generated draw
captures were unavailable offline; this is not evidence every draw qualifies.
When enabled, a bounded aggregate line every 120 presented frames reports joined and
fallback batches, unqualified streams and word-limit batches. Unknown packet
forms must remain fallback until reviewed, not widened speculatively. Profile
submit/suspend/retire timings remain switchable and exclude async CPU-overlap
intervals from retirement polling time. The current work pool minimum is 256 KiB
per original allocation; two simultaneously retained banks increase peak memory.
