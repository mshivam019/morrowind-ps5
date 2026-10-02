#!/usr/bin/env python3
"""Exercise the actual native flip completion functions with scripted VideoOut."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "patches/ps5-draw-batching/platform/ps5_agc_native_runtime.c").read_text()


def function(name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    end = source.index("\n}", start) + 2
    return source[start:end] + "\n"


prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#define AGC_RUNTIME_PACKAGES 1
#define PS5_NATIVE_TITLE_RUNTIME 1
#define PS5_PROFILE_MARK(i) ((void)0)
static int runtime_video_handle = 1, runtime_video_registered = 1;
static unsigned runtime_present_count, runtime_gpu_present_count;
static int runtime_gpu_present_buffer = -1;
static uint64_t runtime_gpu_present_marker = 99;
static unsigned waits, submissions, status_reads, complete_after;
static int submit_error, status_error, pending_error, wait_error;
static int wrong_marker, pending_at_submit, pending_after_complete;
static int64_t submitted_marker;
static int64_t runtime_next_render_marker(void) { return 100; }
static int submit(int handle, int buffer, uint32_t mode, int64_t marker) {
    assert(handle == 1 && buffer < 2 && mode == 1);
    ++submissions; submitted_marker = marker;
    return submit_error;
}
static int status(int handle, void *data) {
    assert(handle == 1); ++status_reads;
    ((uint64_t *)data)[3] = waits >= complete_after && !wrong_marker
        ? (uint64_t)submitted_marker : 0;
    return status_error;
}
static int pending(int handle) {
    assert(handle == 1);
    if (pending_error) return pending_error;
    if (!submissions) return pending_at_submit;
    return waits < complete_after || pending_after_complete;
}
static int wait_vblank(int handle) {
    assert(handle == 1); ++waits; return wait_error;
}
static struct {
    int (*submit_flip)(int, int, uint32_t, int64_t);
    int (*is_flip_pending)(int);
    int (*wait_vblank)(int);
    int (*get_flip_status)(int, void *);
} runtime_video_api = {submit, pending, wait_vblank, status};
'''

main = r'''
static void reset(void) {
    waits = submissions = status_reads = complete_after = 0;
    submit_error = status_error = pending_error = wait_error = 0;
    wrong_marker = pending_at_submit = pending_after_complete = 0;
    runtime_present_count = runtime_gpu_present_count = 0;
    runtime_gpu_present_buffer = -1;
    runtime_video_api.get_flip_status = status;
    RUNTIME_PRESENT_DIAG_RESET();
}
int main(void) {
    reset(); assert(ps5_agc_gate2_present(0) == 0);
    assert(submissions == 1 && runtime_present_count == 1);
#if PS5_PRESENT_COMPLETION_WAIT
    assert(waits == 0 && status_reads == 1);
#if PS5_LIGHT_DIAGNOSTICS
    assert(runtime_present_diag.cpu_flips == 1 && runtime_present_diag.completed_without_wait == 1);
    assert(runtime_present_diag.marker_waits == 0 && runtime_present_diag.legacy_waits == 0);
#endif
    reset(); complete_after = 2;
    assert(ps5_agc_gate2_present(1) == 0 && waits == 2);
#if PS5_LIGHT_DIAGNOSTICS
    assert(runtime_present_diag.marker_waits == 2 && runtime_present_diag.completed_without_wait == 0);
#endif
    reset(); wrong_marker = 1;
    assert(ps5_agc_gate2_present(0) == -1 && waits == 120);
    assert(runtime_present_count == 0);
#if PS5_LIGHT_DIAGNOSTICS
    assert(runtime_present_diag.timeouts == 1 && runtime_present_diag.marker_waits == 120);
#endif
    reset(); pending_after_complete = 1;
    assert(ps5_agc_gate2_present(0) == -1 && waits == 120);
    reset(); status_error = -7;
    assert(ps5_agc_gate2_present(0) == -7 && waits == 0);
#if PS5_LIGHT_DIAGNOSTICS
    assert(runtime_present_diag.status_errors == 1);
#endif
    reset(); complete_after = 1; wait_error = -9;
    assert(ps5_agc_gate2_present(0) == -9 && waits == 1);
#if PS5_LIGHT_DIAGNOSTICS
    assert(runtime_present_diag.wait_errors == 1);
#endif
    reset(); runtime_video_api.get_flip_status = 0;
    assert(ps5_agc_gate2_present(0) == -1 && submissions == 0);
#else
    assert(waits == 1 && status_reads == 0);
#if PS5_LIGHT_DIAGNOSTICS
    assert(runtime_present_diag.legacy_waits == 1 && runtime_present_diag.completed_without_wait == 0);
#endif
#endif
    reset(); submit_error = -5;
    assert(ps5_agc_gate2_present(0) == -5 && waits == 0);
#if PS5_LIGHT_DIAGNOSTICS
    assert(runtime_present_diag.submit_errors == 1 && runtime_present_diag.cpu_flips == 1);
#endif
    reset(); pending_error = -6;
    assert(ps5_agc_gate2_present(0) == -1 && submissions == 0);
#if PS5_LIGHT_DIAGNOSTICS
    assert(runtime_present_diag.pending_errors == 1);
#endif
    reset(); pending_at_submit = 1;
    assert(ps5_agc_gate2_present(0) == -1 && waits == 120 && submissions == 0);
    reset(); assert(ps5_agc_gate2_present(2) == -1 && submissions == 0);
#ifdef PS5_GPU_PRESENT_BATCH
    reset(); runtime_gpu_present_buffer = 1; submitted_marker = 99;
    assert(ps5_agc_gate2_present(0) == -1);
    assert(runtime_gpu_present_buffer == 1 && runtime_gpu_present_count == 0);
    assert(ps5_agc_gate2_present(1) == 0);
    assert(waits == 0 && submissions == 0 && runtime_gpu_present_buffer == -1);
    assert(runtime_gpu_present_count == 1 && runtime_present_count == 1);
    reset(); runtime_gpu_present_buffer = 0; wrong_marker = 1;
    assert(ps5_agc_gate2_present(0) == -1 && waits == 120);
    assert(runtime_gpu_present_buffer == 0 && runtime_gpu_present_count == 0);
#endif
    verify_state_accounting();
    reset();
    for (unsigned frame = 0; frame < 120; ++frame)
        assert(ps5_agc_gate2_present(frame & 1) == 0);
#if PS5_LIGHT_DIAGNOSTICS
    assert(runtime_present_diag.cpu_flips == 120 && runtime_present_diag.gpu_flips == 0);
#endif
    puts("PASS native presentation: exact marker, pending state, bounded waits, errors, default behavior");
}
'''

# Execute the actual clear/flush blocks; only the hardware flush is intercepted.
start = source.index('    failure_phase = "shaders";') + len('    failure_phase = "shaders";')
clear = source[start:source.index("\n#ifdef AGC_RUNTIME_PACKAGES\n    completion_marker", start)]
start = source.index("    PS5_PROFILE_MARK(4);\n", source.index('failure_phase = "shaders";')) + len("    PS5_PROFILE_MARK(4);\n")
flush = source[start:source.index("    PS5_PROFILE_MARK(5);", start)]
state_test = r'''
#define RUNTIME_DRAW_STATE_BEGIN 0x4000u
#define RUNTIME_DRAW_STATE_END 0x8000u
static unsigned flush_calls;
static const void *flushed[3];
static size_t flush_bytes[3];
static void flush_gpu_data(const void *data, size_t bytes) {
    assert(flush_calls < 3); flushed[flush_calls] = data;
    flush_bytes[flush_calls++] = bytes;
}
static void verify_state_accounting(void) {
    uint8_t memory[0x10000];
    const size_t work_bytes = sizeof(memory);
    uint32_t words[256];
    struct {uint32_t *bottom, *up, *down, *top;} command = {words, words+4, words+244, words+256};
    for (int shader_cached = 0; shader_cached <= 1; ++shader_cached) {
        RUNTIME_PRESENT_DIAG_RESET();
        memset(memory, 0xa5, sizeof(memory));
''' + clear + r'''
        for (size_t i = 0; i < sizeof(memory); ++i)
            assert(memory[i] == ((!shader_cached || (i >= 0x4000 && i < 0x8000)) ? 0 : 0xa5));
        flush_calls = 0;
''' + flush + r'''
        assert(flush_calls == (shader_cached ? 3u : 1u));
        assert(flushed[0] == memory + (shader_cached ? 0x4000 : 0));
        assert(flush_bytes[0] == (shader_cached ? 0x4000 : work_bytes));
        if (shader_cached) {
            assert(flushed[1] == words && flush_bytes[1] == 16);
            assert(flushed[2] == words+244 && flush_bytes[2] == 48);
        }
#if PS5_LIGHT_DIAGNOSTICS
        assert(runtime_present_diag.cached_state_draws == (unsigned)shader_cached);
        assert(runtime_present_diag.state_clear_bytes == (shader_cached ? 0x4000u : 0));
        assert(runtime_present_diag.state_flush_bytes == (shader_cached ? 0x4000u : 0));
#endif
    }
}
'''

out = ROOT / "build/present-completion-tests"
out.mkdir(parents=True, exist_ok=True)
for diagnostic in (0, 1):
    for enabled in (0, 1):
        for gpu_present in (0, 1):
            # The forward declaration shares this name; take the definition instead.
            start = source.index("static int runtime_video_wait_idle(void)\n{")
            functions = source[start:source.index("\n}", start) + 2] + "\n"
            if enabled or gpu_present:
                functions += function("static int runtime_video_wait_marker")
            if gpu_present:
                functions += function("static int runtime_gpu_present_finish")
            functions += function("int ps5_agc_gate2_present")
            diag_start = source.index("/* Counter-only diagnostics:")
            diag_end = source.index("/* End counter-only presentation diagnostics. */", diag_start)
            diagnostics = source[diag_start:diag_end]
            path = out / f"present-{enabled}-{gpu_present}-{diagnostic}"
            path.with_suffix(".c").write_text(prefix + diagnostics + functions + state_test + main)
            flags = [f"-DPS5_PRESENT_COMPLETION_WAIT={enabled}", f"-DPS5_LIGHT_DIAGNOSTICS={diagnostic}"]
            if gpu_present:
                flags.append("-DPS5_GPU_PRESENT_BATCH=1")
            subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-variable", *flags, str(path.with_suffix(".c")),
                            "-o", str(path)], check=True)
            result = subprocess.run([str(path)], check=True, capture_output=True, text=True)
            assert result.stderr.count("[ps5-present-diag]") == diagnostic
            if diagnostic:
                assert "present=120 cumulative=1 cpu_flips=120 gpu_flips=0" in result.stderr
                expected = "completed_without_wait=120" if enabled else "legacy_waits=120"
                assert expected in result.stderr
            print(result.stdout.strip())
