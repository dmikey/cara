#!/usr/bin/env python3
"""Rank whole-run surface-flush buckets; never infer measured frame windows."""
import argparse
import json
import math
from pathlib import Path

FORMATS = ('RGBA8', 'RGB8', 'RGB5A1', 'RGB565', 'RGBA4', 'IA8', 'RG8',
           'I8', 'A8', 'IA4', 'I4', 'A4', 'ETC1', 'ETC1A4', 'D16',
           'unknown_15', 'D24', 'D24S8')


def number(value, name, integer=False):
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or not math.isfinite(value) or value < 0
            or (integer and not isinstance(value, int))):
        raise ValueError(f'{name} must be a finite nonnegative ' + ('integer' if integer else 'number'))
    return value


def surface(value):
    if value is None:
        return None
    if not isinstance(value, dict):
        raise ValueError('surface must be an object')
    result = {key: number(value[key], key, True)
              for key in ('address', 'size', 'format', 'width', 'height', 'stride', 'scale')}
    if not isinstance(value.get('tiled'), bool):
        raise ValueError('surface tiled must be boolean')
    result['tiled'] = value['tiled']
    result['end_exclusive'] = result['address'] + result['size']
    result['format_name'] = FORMATS[result['format']] if result['format'] < len(FORMATS) else 'unknown'
    return result


def summarize(report, frames=None, rank='time', limit=20):
    if not isinstance(report, dict) or not isinstance(report.get('entries'), list):
        raise ValueError('report must contain an entries array')
    if frames is not None and (isinstance(frames, bool) or not isinstance(frames, int) or frames <= 0):
        raise ValueError('frames must be a positive integer matching the entire trace interval')
    if rank not in ('time', 'calls', 'bytes') or not isinstance(limit, int) or isinstance(limit, bool) or limit < 1:
        raise ValueError('invalid rank or limit')
    instrumented = report.get('wait_instrumented', False)
    if not isinstance(instrumented, bool):
        raise ValueError('wait_instrumented must be boolean')
    reasons = report.get('reasons', [])
    if not isinstance(reasons, list) or any(not isinstance(x, str) for x in reasons):
        raise ValueError('reasons must be an array of strings')
    groups = {}
    for entry in report['entries']:
        if not isinstance(entry, dict):
            raise ValueError('entry must be an object')
        reason_id = number(entry.get('reason'), 'reason', True)
        # The producer's 129th bucket combines arbitrary unseen cases. Its
        # placeholder reason/source do not describe the accumulated operations.
        overflow = (entry.get('format') == 0xffffffff and entry.get('surface_address') == 0
                    and entry.get('requested_bytes') == 0 and entry.get('width') == 0
                    and entry.get('height') == 0)
        source = None if overflow else surface(entry.get('source'))
        if source is None and not overflow:
            # Older traces lack allocation metadata; retain only known hints.
            source = dict(address=number(entry.get('surface_address'), 'surface_address', True),
                          format=number(entry.get('format'), 'format', True),
                          allocation_metadata='unavailable')
        destination = None if overflow else surface(entry.get('destination'))
        reason = 'overflow_mixed' if overflow else (reasons[reason_id] if reason_id < len(reasons) else f'unknown_{reason_id}')
        descriptor = dict(reason=reason, source=source, destination=destination)
        key = json.dumps(descriptor, sort_keys=True)
        if key not in groups:
            groups[key] = dict(descriptor, trace_buckets=0, calls=0, readback_bytes=0,
                               wall_ms=0.0, wait_ms=0.0 if instrumented else None,
                               read_ms=0.0 if instrumented else None, pack_ms=0.0,
                               request_sizes_bytes=set(), copy_reject_bits=set())
        group = groups[key]
        group['trace_buckets'] += 1
        for field in ('calls', 'readback_bytes'):
            group[field] += number(entry.get(field), field, True)
        group['wall_ms'] += 1000 * number(entry.get('seconds'), 'seconds')
        for field in ('wait', 'read', 'pack'):
            value = entry.get(field + '_seconds')
            if value is not None:
                number(value, field + '_seconds')
            if (field != 'pack' and not instrumented) or value is None:
                group[field + '_ms'] = None
            elif group[field + '_ms'] is not None:
                group[field + '_ms'] += value * 1000
        if not overflow:
            group['request_sizes_bytes'].add(number(entry.get('requested_bytes'), 'requested_bytes', True))
            group['copy_reject_bits'].add(number(entry.get('copy_reject_bits', 0), 'copy_reject_bits', True))
    rows = list(groups.values())
    total_wall = sum(row['wall_ms'] for row in rows)
    totals = dict(calls=sum(row['calls'] for row in rows),
                  readback_bytes=sum(row['readback_bytes'] for row in rows), wall_ms=total_wall)
    for field in ('wait_ms', 'read_ms', 'pack_ms'):
        totals[field] = (None if (field != 'pack_ms' and not instrumented) or any(row[field] is None for row in rows)
                         else sum(row[field] for row in rows))
    for row in rows:
        row['request_sizes_bytes'] = sorted(row['request_sizes_bytes'])
        row['copy_reject_bits'] = sorted(row['copy_reject_bits'])
        row['wall_fraction'] = row['wall_ms'] / total_wall if total_wall else None
        row['wall_ms_per_call'] = row['wall_ms'] / row['calls'] if row['calls'] else None
        if frames is not None:
            row['per_frame'] = {key: row[key] / frames for key in ('calls', 'readback_bytes', 'wall_ms')}
    sort_key = {'time': 'wall_ms', 'calls': 'calls', 'bytes': 'readback_bytes'}[rank]
    rows.sort(key=lambda row: (-row[sort_key], row['reason'], json.dumps(row['source'], sort_keys=True)))
    result = dict(scope='whole_trace_cumulative_not_measured_windows', wait_instrumented=instrumented,
                  wait_read_status=('unavailable_without_wait_instrumentation' if not instrumented else
                                    'incomplete_fields' if totals['wait_ms'] is None or totals['read_ms'] is None else 'available'),
                  request_sizes_semantics='per-call FlushRegion request sizes, not byte totals or original CPU write sizes',
                  ranges_semantics='source/destination allocation ranges, not exact downloaded byte spans',
                  groups=len(rows), input_buckets=len(report['entries']), totals=totals,
                  rank=rank, rows=rows[:limit])
    if frames is not None:
        result['normalization'] = dict(frames=frames, basis='user_asserted_matching_whole_trace_count')
        totals['per_frame'] = {key: totals[key] / frames for key in ('calls', 'readback_bytes', 'wall_ms')}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path, help='surface-flushes.json')
    parser.add_argument('--rank', choices=('time', 'calls', 'bytes'), default='time')
    parser.add_argument('--limit', type=int, default=20)
    parser.add_argument('--frames', type=int, help='Explicit matching WHOLE trace frame count, including warmup; never measured frames only')
    args = parser.parse_args()
    try:
        print(json.dumps(summarize(json.loads(args.report.read_text()), args.frames, args.rank, args.limit),
                         indent=2, allow_nan=False))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
