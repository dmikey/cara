#!/usr/bin/env python3
"""Summarize sampled thread CPU/wall timings without extrapolating phase totals."""
import argparse
import json
import math
from pathlib import Path


PHASES = ('draw_submit', 'vertex_orphan', 'stream_map', 'stream_unmap',
          'uniform_upload', 'vertex_subimage', 'surface_download', 'surface_read_pixels')
SUFFIXES = ('samples', 'sample_clock_failures', 'sample_wall_ms', 'sample_cpu_ms')


def number(value, label, integer=False):
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or not math.isfinite(value) or value < 0
            or (integer and (not isinstance(value, int)))):
        raise ValueError(f'{label} must be a finite nonnegative '
                         + ('integer' if integer else 'number'))
    return value


def summarize(report, warmup, frames):
    if warmup < 0 or frames <= warmup or warmup % 60 or frames % 60:
        raise ValueError('frames and warmup must be multiples of 60, with frames > warmup >= 0')
    windows = report.get('windows')
    if not isinstance(windows, list):
        raise ValueError('report must contain a windows array')
    selected = []
    previous_end = 0
    for index, window in enumerate(windows):
        if not isinstance(window, dict):
            raise ValueError(f'window {index} must be an object')
        end = number(window.get('end_frame'), f'window {index} end_frame', True)
        count = number(window.get('frames'), f'window {index} frames', True)
        if count != 60 or end % 60 or end <= previous_end:
            raise ValueError('windows must be ordered, unique 60-frame windows ending at multiples of 60')
        previous_end = end
        if warmup < end <= frames:
            selected.append(window)
    expected = list(range(warmup + 60, frames + 1, 60))
    if [window['end_frame'] for window in selected] != expected:
        raise ValueError('missing or incomplete measured windows')
    measured = sum(window['frames'] for window in selected)
    if measured != frames - warmup:
        raise ValueError('incorrect measured frame count')
    phases = {}
    for phase in PHASES:
        fields = [f'{phase}_{suffix}' for suffix in SUFFIXES]
        presence = [field in window for window in selected for field in fields]
        if any(presence) and not all(presence):
            raise ValueError(f'{phase}: incomplete sample fields')
        result = dict(N=0, F=0, W=None, C=None, CPU_fraction=None,
                      wall_us_per_call=None, cpu_us_per_call=None,
                      offcpu_us_per_call=None)
        if all(presence):
            totals = [0, 0, 0.0, 0.0]
            for window in selected:
                for index, field in enumerate(fields):
                    totals[index] += number(window[field], field, index < 2)
            n, failures, wall, cpu = totals
            if not all(math.isfinite(value) for value in totals):
                raise ValueError(f'{phase}: nonfinite aggregate')
            if n == 0 and (wall != 0 or cpu != 0):
                raise ValueError(f'{phase}: time totals without successful samples')
            result.update(N=n, F=failures, W=wall, C=cpu)
            if n:
                result.update(wall_us_per_call=1000 * wall / n,
                              cpu_us_per_call=1000 * cpu / n,
                              offcpu_us_per_call=1000 * (wall - cpu) / n,
                              CPU_fraction=cpu / wall if wall else None)
        phases[phase] = result
    return dict(warmup_frames=warmup, target_frames=frames, measured_frames=measured,
                windows=len(selected), units={'W': 'ms', 'C': 'ms'}, phases=phases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path, help='frame-timing.json')
    parser.add_argument('--warmup', type=int, default=300)
    parser.add_argument('--frames', type=int, default=1200)
    args = parser.parse_args()
    try:
        report = json.loads(args.report.read_text())
        if not isinstance(report, dict):
            raise ValueError('report must be a JSON object')
        result = summarize(report, args.warmup, args.frames)
        print(json.dumps(result, indent=2, allow_nan=False))
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
