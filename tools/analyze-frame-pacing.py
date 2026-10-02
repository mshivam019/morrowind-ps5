#!/usr/bin/env python3
"""Analyze sampled engine timings and driver profile windows without console access."""
import argparse
import json
import math
from pathlib import Path
import re
import statistics

FIELDS = re.compile(r'([A-Za-z_][A-Za-z_0-9]*)=([-+]?\d+(?:\.\d+)?)')
ENGINE = re.compile(r'PS5 frame timing ([0-9a-fA-F]+)\s+(.*)')


def percentile(values, fraction):
    values = sorted(values)
    index = (len(values)-1)*fraction
    low, high = math.floor(index), math.ceil(index)
    return values[low] + (values[high]-values[low])*(index-low)


def analyze(text):
    samples, steady_samples, windows = [], [], []
    rejection_signatures = set()
    native = None
    light_latest, light_resets, light_records = {}, {}, {}
    for line in text.splitlines():
        for marker in ('ps5-present-diag', 'ps5-batch-breaks'):
            if '['+marker+']' not in line:
                continue
            fields = FIELDS.findall(line)
            if any('.' in value or value.startswith('-') for key,value in fields):
                continue
            counters = {key:int(value) for key,value in fields}
            if counters.get('cumulative') != 1 or 'present' not in counters:
                continue
            previous = light_latest.get(marker)
            if previous and counters['present'] < previous['present']:
                light_resets[marker] = light_resets.get(marker,0)+1
            light_latest[marker] = counters
            light_records[marker] = light_records.get(marker,0)+1
        engine = ENGINE.search(line)
        if engine:
            values = {k: float(v) for k,v in FIELDS.findall(engine[2])}
            if all(k in values and math.isfinite(values[k]) and values[k]>=0
                   for k in ('update_ms','render_ms','lua_wait_ms')):
                total = sum(values[k] for k in ('update_ms','render_ms','lua_wait_ms'))
                samples.append(total)
                if engine[1] not in ('1','2','3'):
                    steady_samples.append(total)
        if '[ps5-native-profile]' in line:
            native = {k: float(v) for k,v in FIELDS.findall(line)}
        if '[ps5-cpu-profile]' in line:
            values = {k: float(v) for k,v in FIELDS.findall(line)}
            present = values.get('present',0)
            wall = values.get('wall_ms',0)
            # Reporter prints each frame through120, then every60. wall_ms is
            # since its last emitted report, not the last line available in a file.
            # Missing log lines therefore must not change the frame denominator.
            frames = 1 if 1 <= present <=120 else 60 if present>120 and present%60==0 else 0
            if frames and wall>0 and math.isfinite(wall):
                item = dict(present=int(present), frames=frames, wall_ms=wall,
                            interval_mean_ms=wall/frames, interval_fps=frames*1000/wall)
                item['cpu_per_frame']={k:v/frames for k,v in values.items() if k not in ('present','wall_ms')}
                if native and native.get('present')==present:
                    item['native_per_frame']={k:v/frames for k,v in native.items() if k!='present'}
                windows.append(item)
            native = None
        if '[ps5-outer-batch-reject]' in line or '[ps5-batch-texture-reject]' in line:
            rejection_signatures.add(line[line.index('[ps5-'):])
    result = dict(
        limitations=['Engine timing percentiles describe logged sample frames only, not all frames.',
                     'Driver windows are interval averages; they cannot measure individual-frame jitter.',
                     'CPU/native stage times overlap; do not add nested stage totals.',
                     'Rejection receipts are bounded signatures, not occurrence counts.',
                     'Light diagnostics are cumulative counters, not timing windows. Present-counter decreases mark reset boundaries; earlier discarded resets cannot be recovered.',
                     'Screen present counts queue-present boundaries, not successful native presents; queued boundaries are eligibility before queue outcome. Batch counts are nonempty submit attempts before retirement, not guaranteed successful completions. Native counters reset with video release.'],
        engine_sample_count=len(samples), driver_window_count=len(windows),
        rejection_signatures=sorted(rejection_signatures),
        latest_light_diagnostics=light_latest,
        light_diagnostic_reset_boundaries=light_resets,
        light_diagnostic_record_counts=light_records)
    if samples:
        result['sampled_engine_ms']=dict(median=statistics.median(samples),p95=percentile(samples,.95),maximum=max(samples))
    if steady_samples:
        result['sampled_engine_after_first_three_frames_ms']=dict(
            count=len(steady_samples),median=statistics.median(steady_samples),
            p95=percentile(steady_samples,.95),maximum=max(steady_samples))
    if windows:
        frames=sum(w['frames'] for w in windows);wall=sum(w['wall_ms'] for w in windows)
        result['driver_interval_summary']=dict(frames=frames,wall_ms=wall,weighted_fps=frames*1000/wall,
                                               maximum_interval_mean_ms=max(w['interval_mean_ms'] for w in windows))
        result['latest_driver_window']=windows[-1]
        steady = [w for w in windows if w['present']>120]
        if steady:
            frames=sum(w['frames'] for w in steady);wall=sum(w['wall_ms'] for w in steady)
            result['driver_after_present_120_summary']=dict(
                frames=frames,wall_ms=wall,weighted_fps=frames*1000/wall,
                maximum_interval_mean_ms=max(w['interval_mean_ms'] for w in steady))
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs',nargs='+',type=Path)
    args=parser.parse_args()
    # Keep files independent: concatenated logs may duplicate snapshots or launches.
    print(json.dumps({str(p):analyze(p.read_text(errors='replace')) for p in args.logs},indent=2))


if __name__=='__main__':main()
