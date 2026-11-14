#!/usr/bin/env python3
"""Summarize complete cumulative CARA_DRAW_COST_PROFILE log snapshots.

Reports instrumented host draw-submission CPU/wall samples by submitted vertex
count. These are not hardware GPU timings or uninstrumented gameplay results.
"""
import argparse
import json
from pathlib import Path
import re

PATHS = ('accelerated_arrays', 'accelerated_elements', 'feedback_arrays',
         'software_arrays', 'desktop_range')
LIMITS = (64, 256, 1024, 4096, 0)
KEYS = {(path, limit) for path in PATHS for limit in LIMITS}
COUNTERS = ('full_calls', 'full_vertices', 'attempted_samples', 'samples',
            'sampled_vertices', 'cpu_ns', 'wall_ns', 'clock_failures')
ROW_FIELDS = {'session', 'snapshot', 'total_calls', 'path', 'bucket_max', *COUNTERS}
OPTIONS_MARKER = 'PICA draw cost options '
ROW_MARKER = 'PICA draw cost '


def fields(text, required, line):
    result = {}
    for token in text.split():
        match = re.fullmatch(r'([a-z_]+)=([a-z_]+|[0-9]+)', token)
        if not match:
            raise ValueError(f'line {line}: malformed token {token!r}')
        key, value = match.groups()
        if key in result:
            raise ValueError(f'line {line}: duplicate field {key}')
        result[key] = value if key == 'path' else int(value) if value.isdecimal() else value
    if result.keys() != required:
        raise ValueError(f'line {line}: fields differ; missing={sorted(required-result.keys())}, '
                         f'unknown={sorted(result.keys()-required)}')
    for key, value in result.items():
        if key != 'path' and not isinstance(value, int):
            raise ValueError(f'line {line}: {key} must be a nonnegative integer')
    return result


def check_bucket(row, context):
    for key in COUNTERS:
        if row[key] < 0:
            raise ValueError(f'{context}: negative {key}')
    if row['attempted_samples'] > row['full_calls']:
        raise ValueError(f'{context}: attempted samples exceed full calls')
    if row['samples'] + row['clock_failures'] != row['attempted_samples']:
        raise ValueError(f'{context}: samples plus clock failures differ from attempts')
    if row['sampled_vertices'] > row['full_vertices']:
        raise ValueError(f'{context}: sampled vertices exceed full vertices')
    if row['samples'] == 0 and any(row[k] for k in ('sampled_vertices', 'cpu_ns', 'wall_ns')):
        raise ValueError(f'{context}: timing or sampled vertices present without valid samples')
    limit = row['bucket_max']
    minimum = {64: 0, 256: 65, 1024: 257, 4096: 1025, 0: 4097}[limit]
    for count, vertices in [('full_calls', 'full_vertices'), ('samples', 'sampled_vertices')]:
        if row[vertices] < row[count] * minimum:
            raise ValueError(f'{context}: vertices below bucket minimum')
        if limit and row[vertices] > row[count] * limit:
            raise ValueError(f'{context}: vertices exceed bucket maximum')
        if row[count] == 0 and row[vertices]:
            raise ValueError(f'{context}: vertices without calls')
    # CPU includes wall-clock read overhead. CPU > wall is valid for tiny calls.


def parse_sessions(text):
    sessions = []
    current = None
    for line_number, line in enumerate(text.splitlines(), 1):
        if OPTIONS_MARKER in line:
            options = fields(line.split(OPTIONS_MARKER, 1)[1].strip(),
                             {'session', 'enabled', 'sample_rate', 'log_calls'}, line_number)
            if options['session'] < 1 or options['enabled'] != 1:
                raise ValueError(f'line {line_number}: invalid enabled session')
            if options['sample_rate'] != 32 or options['log_calls'] != 65536:
                raise ValueError(f'line {line_number}: unsupported profiler sampling configuration')
            current = {'index': len(sessions)+1, 'options_line': line_number,
                       'options': options, 'snapshots': []}
            sessions.append(current)
        elif ROW_MARKER in line:
            if current is None:
                raise ValueError(f'line {line_number}: snapshot precedes session options; '
                                 'provide the complete session log')
            row = fields(line.split(ROW_MARKER, 1)[1].strip(), ROW_FIELDS, line_number)
            if row['session'] != current['options']['session']:
                raise ValueError(f'line {line_number}: row crosses session boundary')
            key = (row['path'], row['bucket_max'])
            if key not in KEYS:
                raise ValueError(f'line {line_number}: unknown path/bucket {key}')
            check_bucket(row, f'line {line_number}')
            snapshots = current['snapshots']
            if not snapshots or row['snapshot'] != snapshots[-1]['snapshot']:
                expected = snapshots[-1]['snapshot']+1 if snapshots else 1
                if row['snapshot'] != expected:
                    raise ValueError(f'line {line_number}: expected snapshot {expected}')
                snapshots.append({'snapshot': row['snapshot'], 'first_line': line_number,
                                  'total_calls': row['total_calls'], 'rows': {}})
            snapshot = snapshots[-1]
            if row['total_calls'] != snapshot['total_calls']:
                raise ValueError(f'line {line_number}: inconsistent snapshot total calls')
            if key in snapshot['rows']:
                raise ValueError(f'line {line_number}: duplicate snapshot path/bucket {key}')
            snapshot['rows'][key] = row
    if not sessions:
        raise ValueError('no draw-cost options session marker found')
    for session in sessions:
        previous = None
        for snapshot in session['snapshots']:
            if snapshot['rows'].keys() != KEYS:
                raise ValueError(f"session {session['index']} snapshot {snapshot['snapshot']}: "
                                 'incomplete 25-row snapshot')
            total = sum(row['full_calls'] for row in snapshot['rows'].values())
            expected = snapshot['snapshot'] * session['options']['log_calls']
            if total != snapshot['total_calls'] or total != expected:
                raise ValueError('snapshot full-call totals do not match cumulative snapshot boundary')
            if previous:
                for key, row in snapshot['rows'].items():
                    for field in COUNTERS:
                        if row[field] < previous['rows'][key][field]:
                            raise ValueError(f'snapshot {snapshot["snapshot"]}: {key} {field} decreased')
            previous = snapshot
    return sessions


def ratio(numerator, denominator):
    return numerator / denominator if denominator else None


def summarize(text, session_index=None, first_snapshot=None, last_snapshot=None):
    sessions = parse_sessions(text)
    if session_index is None:
        if len(sessions) != 1:
            raise ValueError(f'found {len(sessions)} sessions; select one with --session')
        session_index = 1
    if not 1 <= session_index <= len(sessions):
        raise ValueError(f'--session must be between 1 and {len(sessions)}')
    session = sessions[session_index-1]
    snapshots = session['snapshots']
    if len(snapshots) < 2:
        raise ValueError('need at least two complete snapshots in the selected session')
    first_snapshot = snapshots[0]['snapshot'] if first_snapshot is None else first_snapshot
    last_snapshot = snapshots[-1]['snapshot'] if last_snapshot is None else last_snapshot
    if not 1 <= first_snapshot < last_snapshot <= snapshots[-1]['snapshot']:
        raise ValueError('--first-snapshot/--last-snapshot must select increasing available boundaries')
    first, last = snapshots[first_snapshot-1], snapshots[last_snapshot-1]
    buckets = []
    totals = dict.fromkeys(COUNTERS, 0)
    for path in PATHS:
        for limit in LIMITS:
            key = (path, limit)
            delta = {field: last['rows'][key][field]-first['rows'][key][field] for field in COUNTERS}
            check_bucket(dict(delta, bucket_max=limit), f'interval {path}/{limit}')
            for field in COUNTERS:
                totals[field] += delta[field]
            samples, vertices, calls = delta['samples'], delta['sampled_vertices'], delta['full_calls']
            buckets.append({
                'path': path, 'vertex_bucket': f'<= {limit}' if limit else '> 4096',
                'counters': delta,
                'sample_call_coverage_percent': ratio(100*samples, calls),
                'sample_vertex_coverage_percent': ratio(100*vertices, delta['full_vertices']),
                'full_mean_submitted_vertices_per_call': ratio(delta['full_vertices'], calls),
                'sample_mean_submitted_vertices_per_call': ratio(vertices, samples),
                'sample_mean_cpu_us_per_call': ratio(delta['cpu_ns']/1000, samples),
                'sample_mean_wall_us_per_call': ratio(delta['wall_ns']/1000, samples),
                'sample_cpu_ns_per_submitted_vertex': ratio(delta['cpu_ns'], vertices),
                'sample_wall_ns_per_submitted_vertex': ratio(delta['wall_ns'], vertices),
                'valid_samples': samples,
                'timing_has_clock_errors': bool(delta['clock_failures']),
                'sample_count_below_32': 0 < samples < 32,
            })
    return {
        'session': {key: value for key, value in session.items() if key != 'snapshots'},
        'complete_snapshots': len(snapshots),
        'first_boundary': {key: value for key, value in first.items() if key != 'rows'},
        'last_boundary': {key: value for key, value in last.items() if key != 'rows'},
        'total_counter_delta': totals,
        'buckets': buckets,
        'limitations': [
            'Instrumented host-thread submission timings; not hardware GPU execution or gameplay performance.',
            'CPU intervals include wall-clock read overhead; CPU can exceed wall for tiny calls.',
            'CPU minus wall is not a precise wait-time measurement; no clock overhead is subtracted.',
            'Sampling is approximately 1/32 separately per path/bucket; rare buckets can lack valid samples.',
            'Means have no confidence intervals: the profiler does not record variance or individual samples.',
            'Bucket comparisons also vary shader/state/content; they do not prove causal call-versus-vertex scaling.',
            'No batching compatibility or memory-hazard safety is established by these records.',
            'Differences exclude the first boundary and trailing partial intervals; no per-frame division is performed.',
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--session', type=int, help='One-based session index; required for multi-session logs')
    parser.add_argument('--first-snapshot', type=int, help='First cumulative boundary, excluded from interval')
    parser.add_argument('--last-snapshot', type=int, help='Last cumulative boundary, included in interval')
    args = parser.parse_args()
    try:
        report = summarize(args.log.read_text(errors='replace'), args.session,
                           args.first_snapshot, args.last_snapshot)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
