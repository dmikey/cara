#!/usr/bin/env python3
"""Validate and rank bounded successful surface-copy traces; no FPS extrapolation."""
import argparse
import json
from pathlib import Path


TOTAL_FIELDS = ('calls', 'guest_bytes', 'wall_ns', 'thread_cpu_ns', 'wall_samples',
                'cpu_samples', 'wall_failures', 'cpu_failures', 'integer_saturations')
LAYOUT_FIELDS = ('format', 'type', 'tiled', 'width', 'height', 'stride', 'scale', 'custom')


def integer(value, name, maximum=(1 << 64) - 1):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError(f'{name} must be an unsigned integer <= {maximum}')
    return value


def boolean(value, name):
    if type(value) is not bool:
        raise ValueError(f'{name} must be boolean')
    return value


def totals(value):
    if not isinstance(value, dict):
        raise ValueError('totals must be an object')
    result = {key: integer(value.get(key), key) for key in TOTAL_FIELDS}
    if result['integer_saturations']:
        raise ValueError('saturated totals cannot be exactly reconciled')
    for clock, samples, failures in (('wall_ns', 'wall_samples', 'wall_failures'),
                                      ('thread_cpu_ns', 'cpu_samples', 'cpu_failures')):
        if result[samples] + result[failures] != result['calls']:
            raise ValueError(f'{samples} + {failures} must equal calls')
        if not result[samples] and result[clock]:
            raise ValueError(f'{clock} requires a successful clock sample')
    return result


def add_totals(rows):
    return {key: sum(row[key] for row in rows) for key in TOTAL_FIELDS}


def layout(value):
    if not isinstance(value, dict):
        raise ValueError('surface layout must be an object')
    return {key: (boolean(value.get(key), key) if key in ('tiled', 'custom') else
                  integer(value.get(key), key, (1 << 32) - 1)) for key in LAYOUT_FIELDS}


def interval(value, name):
    if not isinstance(value, list) or len(value) != 2:
        raise ValueError(f'{name} must be a half-open address pair')
    start, end = (integer(x, name, (1 << 32) - 1) for x in value)
    if end <= start:
        raise ValueError(f'{name} must be nonempty and ordered')
    return start, end


def summarize(report, rank='time', limit=64):
    if rank not in ('time', 'cpu', 'calls', 'bytes') or type(limit) is not int or limit <= 0:
        raise ValueError('invalid ranking or row limit')
    if not isinstance(report, dict) or report.get('kind') != 'returned_validate_surface_copies':
        raise ValueError('expected a returned surface-validation-copy report')
    if type(report.get('schema_version')) is not int or report['schema_version'] != 1:
        raise ValueError('unsupported schema version')
    if report.get('enabled') is not True or report.get('common_counters_required') is not False:
        raise ValueError('expected enabled counters-independent tracing')
    if report.get('frame_clock') != 'VideoCore::GetCurrentFrame':
        raise ValueError('unexpected frame clock')
    start = integer(report.get('window_start_inclusive'), 'start', (1 << 32) - 1)
    end = integer(report.get('window_end_exclusive'), 'end', (1 << 32) - 1)
    integer(report.get('written_frame'), 'written_frame', (1 << 32) - 1)
    caps = {key: integer(report.get(key), key) for key in
            ('detail_capacity', 'identity_capacity', 'bucket_capacity')}
    if any(not x for x in caps.values()):
        raise ValueError('trace capacities must be positive')
    for key in ('allocation_events', 'untracked_allocations', 'unknown_identity_uses',
                'validation_invocations_all_frames', 'invalidation_calls_all_frames'):
        integer(report.get(key), key)
    full = totals(report.get('totals'))
    detail_overflow = totals(report.get('detail_overflow'))
    bucket_overflow = totals(report.get('bucket_overflow'))
    if full['calls']:
        low = integer(report.get('actual_min_frame'), 'min_frame', (1 << 32) - 1)
        high = integer(report.get('actual_max_frame'), 'max_frame', (1 << 32) - 1)
        if not start <= low <= high < end:
            raise ValueError('observed copy frames must lie within the trace window')
    elif report.get('actual_min_frame') is not None or report.get('actual_max_frame') is not None:
        raise ValueError('an empty trace must have null frame endpoints')
    details, buckets = report.get('details'), report.get('buckets')
    if not isinstance(details, list) or not isinstance(buckets, list):
        raise ValueError('details and buckets must be arrays')
    if len(details) != min(caps['detail_capacity'], full['calls']):
        raise ValueError('detail count does not match chronological capture capacity')
    if len(buckets) > caps['bucket_capacity']:
        raise ValueError('bucket count exceeds capacity')
    if bucket_overflow['calls'] and len(buckets) != caps['bucket_capacity']:
        raise ValueError('bucket overflow requires all bucket slots occupied')
    detail_totals, invalidations, unsafe_sources = [], {0: 0, 1: 0, 2: 0}, 0
    for event in details:
        if not isinstance(event, dict):
            raise ValueError('detail must be an object')
        frame = integer(event.get('frame'), 'detail frame', (1 << 32) - 1)
        if not low <= frame <= high:
            raise ValueError('detail frame outside observed bounds')
        for key in ('invocation', 'fragment'):
            if not integer(event.get(key), key):
                raise ValueError(f'{key} must be positive')
        requested = interval(event.get('requested'), 'requested')
        invalid = interval(event.get('invalid'), 'invalid')
        actual = interval(event.get('actual'), 'actual')
        if not requested[0] <= invalid[0] < invalid[1] <= requested[1]:
            raise ValueError('invalid fragment must be inside the validation request')
        if max(actual[0], invalid[0]) >= min(actual[1], invalid[1]):
            raise ValueError('returned copy does not advance the invalid fragment')
        if integer(event.get('guest_bytes'), 'detail bytes') != actual[1] - actual[0]:
            raise ValueError('detail bytes must equal actual copied interval length')
        fill = boolean(event.get('fill'), 'fill')
        unsafe_sources += not boolean(event.get('source_valid_before_copy'), 'source validity')
        for side in ('src', 'dst'):
            source = event.get(side)
            layout(source)
            address = integer(source.get('address'), 'surface address', (1 << 32) - 1)
            last = integer(source.get('end'), 'surface end', (1 << 32) - 1)
            if not address <= actual[0] < actual[1] <= last:
                raise ValueError('copy interval must lie inside both surface allocations')
            integer(source.get('handle'), 'handle', (1 << 32) - 1)
            boolean(source.get('registered'), 'registered')
            identity = source.get('allocation_id')
            created = source.get('created_frame')
            if identity is None:
                if created is not None:
                    raise ValueError('unknown allocation must have unknown creation frame')
            else:
                if not integer(identity, 'allocation_id'):
                    raise ValueError('known allocation ID must be positive')
                integer(created, 'created_frame', (1 << 32) - 1)
            serial = source.get('last_observed_invalidation_serial')
            reason = integer(source.get('invalidation_reason'), 'invalidation reason', 2)
            if serial is None:
                if reason:
                    raise ValueError('unknown invalidation must have unknown reason')
            elif not integer(serial, 'invalidation serial') or not reason:
                raise ValueError('observed invalidation requires positive serial and reason')
            if side == 'dst':
                invalidations[reason] += 1
            rectangle = event.get(side + '_scaled_rect')
            if side == 'src' and fill:
                if rectangle is not None:
                    raise ValueError('fill source has no texture rectangle')
            elif not isinstance(rectangle, list) or len(rectangle) != 4:
                raise ValueError('scaled rectangle must contain four coordinates')
            else:
                for coordinate in rectangle:
                    integer(coordinate, 'rectangle coordinate', (1 << 32) - 1)
        row = dict.fromkeys(TOTAL_FIELDS, 0)
        row.update(calls=1, guest_bytes=event['guest_bytes'])
        for field, samples, failures in (('wall_ns', 'wall_samples', 'wall_failures'),
                                          ('thread_cpu_ns', 'cpu_samples', 'cpu_failures')):
            elapsed = event.get(field)
            if elapsed is None:
                row[failures] = 1
            else:
                row[field] = integer(elapsed, field)
                row[samples] = 1
        detail_totals.append(row)
    if add_totals(detail_totals + [detail_overflow]) != full:
        raise ValueError('chronological details + overflow do not reconcile with full totals')
    rows, keys = [], set()
    for bucket in buckets:
        if not isinstance(bucket, dict):
            raise ValueError('bucket must be an object')
        descriptor = dict(fill=boolean(bucket.get('fill'), 'bucket fill'),
                          src=layout(bucket.get('src')), dst=layout(bucket.get('dst')))
        key = json.dumps(descriptor, sort_keys=True)
        if key in keys:
            raise ValueError('duplicate aggregate layout bucket')
        keys.add(key)
        values = totals(bucket.get('totals'))
        if not values['calls']:
            raise ValueError('occupied bucket must contain a returned copy')
        rows.append(dict(descriptor, **values))
    if add_totals(rows + [bucket_overflow]) != full:
        raise ValueError('layout buckets + overflow do not reconcile with full totals')
    field = dict(time='wall_ns', cpu='thread_cpu_ns', calls='calls', bytes='guest_bytes')[rank]
    rows.sort(key=lambda row: (-row[field], json.dumps(row, sort_keys=True)))
    return dict(scope='successful_ValidateSurface_CopySurface_returns_only',
                timing='instrumented_host_enqueue_or_wait_not_gpu_completion',
                window=dict(start_inclusive=start, end_exclusive=end),
                totals=full, bucket_overflow=bucket_overflow, detail_overflow=detail_overflow,
                aggregate_buckets=len(rows), rows=rows[:limit], rank=rank,
                chronological_details=dict(count=len(details), unbiased_sample=False,
                    destination_last_observed_reason=invalidations,
                    invalid_sources_before_copy=unsafe_sources),
                limitations=['No skipped-copy or cache-reuse safety conclusion follows from counts.',
                    'Chronological details can omit later addresses and invalidation histories.',
                    'Logical guest bytes are copied spans, not host memory-bus traffic.',
                    'Clock and tracing overhead remain; no speedup or FPS estimate is inferred.'])


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON key: {key}')
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--rank', choices=('time', 'cpu', 'calls', 'bytes'), default='time')
    parser.add_argument('--limit', type=int, default=64)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        report = json.loads(args.report.read_text(), object_pairs_hook=unique_object)
        result = summarize(report, args.rank, args.limit)
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(2, f'Invalid surface-copy report: {error}\n')
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
