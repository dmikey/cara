#!/usr/bin/env python3
"""Summarize opt-in PICA command diagnostics from one complete log session.

These instrumented CPU timings include diagnostic overhead. They are neither
hardware GPU timings nor uninstrumented gameplay performance measurements.
"""
import argparse
import json
from pathlib import Path
import re

OPTIONS_MARKER = 'PICA command options '
PROFILE_MARKER = 'PICA command profile '
FIELDS = (
    'lists', 'writes', 'draws', 'list_cpu_ns', 'draw_cpu_ns', 'notifications',
    'repeat_shader_notifications', 'code_words', 'repeat_code_words',
    'swizzle_words', 'repeat_swizzle_words', 'clock_failures',
)
REPEAT_PAIRS = (
    ('repeat_shader_notifications', 'notifications'),
    ('repeat_code_words', 'code_words'),
    ('repeat_swizzle_words', 'swizzle_words'),
)


def parse_record(text, line_number):
    fields = {}
    for token in text.split():
        match = re.fullmatch(r'([a-z_]+)=([0-9]+)', token)
        if not match:
            raise ValueError(f'line {line_number}: malformed profile token {token!r}')
        name, value = match.groups()
        if name in fields:
            raise ValueError(f'line {line_number}: duplicate field {name}')
        fields[name] = int(value)
    missing = set(FIELDS) - fields.keys()
    if missing:
        raise ValueError(f'line {line_number}: missing fields {sorted(missing)}')
    unknown = fields.keys() - set(FIELDS)
    if unknown:
        raise ValueError(f'line {line_number}: unsupported fields {sorted(unknown)}')
    if fields['draw_cpu_ns'] > fields['list_cpu_ns']:
        raise ValueError(f'line {line_number}: nested draw CPU exceeds inclusive list CPU')
    for repeated, total in REPEAT_PAIRS:
        if fields[repeated] > fields[total]:
            raise ValueError(f'line {line_number}: {repeated} exceeds {total}')
    return {'line': line_number, 'counters': fields}


def read_sessions(log_text):
    sessions = []
    current = None
    for line_number, line in enumerate(log_text.splitlines(), 1):
        if OPTIONS_MARKER in line:
            options = line.split(OPTIONS_MARKER, 1)[1].strip()
            match = re.fullmatch(
                r'repeat_state=(true|false|0|1) command_profile=(true|false|0|1)', options)
            if not match:
                raise ValueError(f'line {line_number}: malformed command options marker')
            current = {
                'session': len(sessions) + 1,
                'options_line': line_number,
                'repeat_state': match[1] in ('true', '1'),
                'command_profile': match[2] in ('true', '1'),
                'records': [],
            }
            sessions.append(current)
        if PROFILE_MARKER in line:
            if current is None:
                raise ValueError(f'line {line_number}: profile precedes session options marker; '
                                 'provide the complete session log')
            if not current['command_profile']:
                raise ValueError(f'line {line_number}: profile appears in a disabled session')
            current['records'].append(parse_record(
                line.split(PROFILE_MARKER, 1)[1].strip(), line_number))
    if not sessions:
        raise ValueError('no PICA command options session marker found')
    return sessions


def summarize(log_text, session_number=None, matched_frames=None, matched_lists=None):
    sessions = read_sessions(log_text)
    if session_number is None:
        if len(sessions) != 1:
            raise ValueError(f'found {len(sessions)} sessions; select exactly one with --session')
        session_number = 1
    if not 1 <= session_number <= len(sessions):
        raise ValueError(f'session must be between 1 and {len(sessions)}')
    session = sessions[session_number - 1]
    records = session['records']
    if len(records) < 2:
        raise ValueError('need at least two complete cumulative records in the selected session')
    for previous, current in zip(records, records[1:]):
        before, after = previous['counters'], current['counters']
        if after['lists'] <= before['lists']:
            raise ValueError(f"line {current['line']}: lists did not advance; possible session reset")
        for field in FIELDS:
            if after[field] < before[field]:
                raise ValueError(f"line {current['line']}: {field} decreased; possible session reset")
    first, last = records[0]['counters'], records[-1]['counters']
    delta = {field: last[field] - first[field] for field in FIELDS}
    if delta['draw_cpu_ns'] > delta['list_cpu_ns']:
        raise ValueError('interval nested draw CPU exceeds inclusive list CPU')
    for repeated, total in REPEAT_PAIRS:
        if delta[repeated] > delta[total]:
            raise ValueError(f'interval {repeated} exceeds {total}')
    remainder = delta['list_cpu_ns'] - delta['draw_cpu_ns']
    timing = {
        'inclusive_command_list_cpu_ms': delta['list_cpu_ns'] / 1e6,
        'nested_draw_cpu_ms': delta['draw_cpu_ns'] / 1e6,
        'command_list_cpu_remainder_ms': remainder / 1e6,
        'nested_draw_percent_of_inclusive_list_cpu': (
            100 * delta['draw_cpu_ns'] / delta['list_cpu_ns']
            if delta['list_cpu_ns'] else None),
        'clock_failures': delta['clock_failures'],
        'cpu_timing_valid': delta['clock_failures'] == 0,
    }
    if (matched_frames is None) != (matched_lists is None):
        raise ValueError('--matched-frames and --matched-lists must be supplied together')
    if matched_frames is not None:
        if matched_frames <= 0:
            raise ValueError('--matched-frames must be positive')
        if tuple(matched_lists) != (first['lists'], last['lists']):
            raise ValueError('--matched-lists does not match first/last selected cumulative records')
        if delta['clock_failures']:
            raise ValueError('cannot report per-frame CPU timing with interval clock failures')
        timing['explicitly_matched_frames'] = matched_frames
        timing['inclusive_command_list_cpu_ms_per_matched_frame'] = (
            delta['list_cpu_ns'] / matched_frames / 1e6)
        timing['nested_draw_cpu_ms_per_matched_frame'] = (
            delta['draw_cpu_ns'] / matched_frames / 1e6)
        timing['command_list_cpu_remainder_ms_per_matched_frame'] = remainder / matched_frames / 1e6
    return {
        'session': {key: value for key, value in session.items() if key != 'records'},
        'complete_records': len(records),
        'first': records[0],
        'last': records[-1],
        'counter_delta': delta,
        'cpu_timing': timing,
        'repeated_write_percentages': {
            repeated: 100 * delta[repeated] / delta[total] if delta[total] else None
            for repeated, total in REPEAT_PAIRS
        },
        'limitations': [
            'Instrumented CPU diagnostics include counter, clock and logging overhead.',
            'Draw CPU is nested within inclusive command-list CPU; do not add them.',
            'The remainder includes non-draw command processing and diagnostic overhead.',
            'These are neither hardware GPU timings nor uninstrumented gameplay performance.',
            'Endpoints omit partial intervals; total capture frames are not matched interval frames.',
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path, help='Complete emulator log including command options marker')
    parser.add_argument('--session', type=int, help='One-based session index, required for multi-session logs')
    parser.add_argument('--matched-frames', type=int,
                        help='Explicitly established frames between the selected first/last list records')
    parser.add_argument('--matched-lists', type=int, nargs=2, metavar=('FIRST', 'LAST'),
                        help='Confirm exact cumulative list endpoints for --matched-frames')
    args = parser.parse_args()
    try:
        report = summarize(args.log.read_text(errors='replace'), args.session,
                           args.matched_frames, args.matched_lists)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
