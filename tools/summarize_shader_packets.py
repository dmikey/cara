#!/usr/bin/env python3
"""Summarize complete cumulative VS shader-packet census snapshots.

Counts and percentages describe packet frequencies, never CPU savings or frame
performance. The separate legacy PICA command-profile parser is unchanged.

Usage: python3 tools/summarize_shader_packets.py LOG [--session ID]
       [--snapshots FIRST LAST]
Endpoints are exact cumulative list counters; session is the logged uint64 ID.
Requires at least two complete snapshots. Missing, malformed or reset data fails
closed. Synthetic protocol tests: python3 -m unittest discover -s tests
-p test_summarize_shader_packets.py
"""
import argparse
import json
from pathlib import Path
import re

MARKER = 'PICA VS packet '
MAX_U64 = (1 << 64) - 1
KINDS = ('program', 'swizzle')
KEYS = tuple((kind, grouped) for kind in KINDS for grouped in (0, 1))
COUNTERS = ('packets', 'extra_words', 'complete_packets',
            'repeated_capacity_packets', 'eligible_packets', 'eligible_extra_words')
MASKS = ('exclusive_packets', 'mirrored_packets') + tuple(f'm{i}' for i in range(16))
BOUNDS = ((0, 0), (1, 1), (2, 3), (4, 7), (8, 15), (16, 31),
          (32, 63), (64, 127), (128, 511), (512, 2047))


def record(text, fields, line):
    result = {}
    for token in text.split():
        match = re.fullmatch(r'([a-z][a-z0-9_]*)=([^\s]+)', token)
        if not match:
            raise ValueError(f'line {line}: malformed census token {token!r}')
        name, value = match.groups()
        if name in result:
            raise ValueError(f'line {line}: duplicate field {name}')
        if name == 'kind':
            if value not in KINDS:
                raise ValueError(f'line {line}: unsupported kind {value!r}')
        else:
            if not re.fullmatch(r'[0-9]+', value) or int(value) > MAX_U64:
                raise ValueError(f'line {line}: invalid uint64 field {name}')
            value = int(value)
        result[name] = value
    if set(result) != set(fields):
        raise ValueError(f'line {line}: census fields missing={sorted(set(fields)-result.keys())} '
                         f'unsupported={sorted(result.keys()-set(fields))}')
    if 'grouped' in result and result['grouped'] not in (0, 1):
        raise ValueError(f'line {line}: grouped must be 0 or 1')
    if 'bucket' in result and result['bucket'] >= len(BOUNDS):
        raise ValueError(f'line {line}: unsupported length bucket')
    return result


def check_counts(counts, grouped, where, bucket=None):
    p, w, c, f, e, ew = (counts[name] for name in COUNTERS)
    if c > p or f > p or e > min(c, f):
        raise ValueError(f'{where}: complete/capacity/eligible subset mismatch')
    lo, hi = BOUNDS[bucket] if bucket is not None else (0, 2047)
    if not lo*p <= w <= hi*p:
        raise ValueError(f'{where}: declared extra words outside packet length bounds')
    if grouped or bucket in (0, 1):
        if e or ew:
            raise ValueError(f'{where}: grouped or short packets cannot be eligible')
    if not max(2, lo)*e <= ew <= hi*e:
        raise ValueError(f'{where}: eligible extra words outside eligible length bounds')
    if not lo*(p-e) <= w-ew <= hi*(p-e):
        raise ValueError(f'{where}: eligible words are not a subset of declared extra words')
    if not grouped and bucket is not None and bucket >= 2 and e < c+f-p:
        raise ValueError(f'{where}: eligible intersection is inconsistent with complete/capacity')


def validate(snapshot, where):
    if set(snapshot['totals']) != set(KEYS) or set(snapshot['masks']) != set(KEYS):
        raise ValueError(f'{where}: incomplete total or mask rows')
    if set(snapshot['lengths']) != {(kind, group, b) for kind, group in KEYS for b in range(10)}:
        raise ValueError(f'{where}: incomplete length bucket rows')
    for key in KEYS:
        total = snapshot['totals'][key]
        check_counts(total, key[1], where)
        for b in range(10):
            check_counts(snapshot['lengths'][(*key, b)], key[1], where, b)
        for field in COUNTERS:
            if sum(snapshot['lengths'][(*key, b)][field] for b in range(10)) != total[field]:
                raise ValueError(f'{where}: length bucket sum disagrees with total {field}')
        masks = snapshot['masks'][key]
        if sum(masks[f'm{i}'] for i in range(16)) != total['packets']:
            raise ValueError(f'{where}: mask sum disagrees with packet total')
        if masks['exclusive_packets'] + masks['mirrored_packets'] != total['packets']:
            raise ValueError(f'{where}: exclusive/mirrored packet sum disagrees with total')
    if sum(x['packets'] for x in snapshot['totals'].values()) > snapshot['all_packets']:
        raise ValueError(f'{where}: VS source packets exceed all parsed headers')


def difference(before, after):
    result = {'all_packets': after['all_packets'] - before['all_packets']}
    if result['all_packets'] < 0:
        raise ValueError('all_packets decreased; possible reset')
    for table, fields in (('totals', COUNTERS), ('lengths', COUNTERS), ('masks', MASKS)):
        result[table] = {}
        for key in before[table]:
            row = {name: after[table][key][name] - before[table][key][name] for name in fields}
            if any(value < 0 for value in row.values()):
                raise ValueError(f'{table} {key} counter decreased; possible reset')
            result[table][key] = row
    validate(result, 'interval')
    return result


def read_sessions(text):
    sessions = {}
    current = None
    for line, raw in enumerate(text.splitlines(), 1):
        if MARKER not in raw:
            continue
        tail = raw.split(MARKER, 1)[1]
        action, separator, tokens = tail.partition(' ')
        if not separator or action not in ('begin', 'total', 'length', 'masks', 'end'):
            raise ValueError(f'line {line}: unsupported census record')
        if action == 'begin':
            row = record(tokens, ('schema', 'session', 'snapshot', 'all_packets'), line)
            if row['schema'] != 1:
                raise ValueError(f'line {line}: unsupported census schema')
            if current is not None:
                raise ValueError(f'line {line}: nested begin before complete snapshot end')
            previous = sessions.get(row['session'], [])
            if previous and row['snapshot'] <= previous[-1]['snapshot']:
                raise ValueError(f'line {line}: snapshot did not advance; duplicate or reset')
            current = {**row, 'begin_line': line, 'totals': {}, 'lengths': {}, 'masks': {}}
            continue
        if current is None:
            raise ValueError(f'line {line}: row precedes complete snapshot begin')
        prefix = ('session', 'snapshot')
        fields = prefix + (('all_packets',) if action == 'end' else
                           ('kind', 'grouped') + (('bucket',) if action == 'length' else ()) +
                           (MASKS if action == 'masks' else COUNTERS))
        row = record(tokens, fields, line)
        if any(row[name] != current[name] for name in prefix):
            raise ValueError(f'line {line}: mixed session/snapshot rows')
        if action == 'end':
            if row['all_packets'] != current['all_packets']:
                raise ValueError(f'line {line}: footer disagrees with snapshot begin')
            current['end_line'] = line
            validate(current, f'line {line}')
            previous = sessions.setdefault(row['session'], [])
            if previous:
                difference(previous[-1], current)
            previous.append(current)
            current = None
        else:
            table = {'total': 'totals', 'length': 'lengths', 'masks': 'masks'}[action]
            key = (row['kind'], row['grouped']) + ((row['bucket'],) if action == 'length' else ())
            if key in current[table]:
                raise ValueError(f'line {line}: duplicate {action} row {key}')
            current[table][key] = {name: row[name] for name in (MASKS if action == 'masks' else COUNTERS)}
    if current is not None:
        raise ValueError('incomplete snapshot at end of log; missing footer')
    if not sessions:
        raise ValueError('packet census unavailable: no complete snapshots; diagnostic may be disabled')
    return sessions


def fraction(numerator, denominator):
    return {'numerator': numerator, 'denominator': denominator,
            'percent': 100*numerator/denominator if denominator else None}


def summarize(text, session=None, snapshots=None):
    sessions = read_sessions(text)
    if session is None:
        if len(sessions) != 1:
            raise ValueError('multiple packet sessions; select an actual session ID with --session')
        session = next(iter(sessions))
    if session not in sessions:
        raise ValueError('selected session ID is unavailable')
    records = sessions[session]
    if len(records) < 2:
        raise ValueError('packet census unavailable: need two complete snapshots in the selected session')
    if snapshots is None:
        first, last = records[0], records[-1]
    else:
        selected = {row['snapshot']: row for row in records}
        if len(snapshots) != 2 or snapshots[0] >= snapshots[1] or any(n not in selected for n in snapshots):
            raise ValueError('--snapshots requires two existing increasing exact snapshot endpoints')
        first, last = (selected[n] for n in snapshots)
    delta = difference(first, last)
    aggregate = {name: sum(row[name] for row in delta['totals'].values()) for name in COUNTERS}
    ungrouped = {name: sum(delta['totals'][(kind, 0)][name] for kind in KINDS) for name in COUNTERS}
    classes = []
    for kind, grouped in KEYS:
        row = delta['totals'][(kind, grouped)]
        classes.append({'kind': kind, 'grouped': bool(grouped), 'counts': row,
                        'mask_and_mirror_packet_counts': delta['masks'][(kind, grouped)],
                        'length_buckets': [{'bucket': b, 'declared_extra_word_range': BOUNDS[b],
                                            **delta['lengths'][(kind, grouped, b)]} for b in range(10)],
                        'eligible_packet_frequency': fraction(row['eligible_packets'], row['packets']),
                        'eligible_extra_word_frequency': fraction(row['eligible_extra_words'], row['extra_words'])})
    return {
        'available': True, 'schema': 1, 'session': session,
        'complete_session_snapshots': len(records),
        'endpoints': [{'snapshot': row['snapshot'], 'all_packets': row['all_packets'],
                       'begin_line': row['begin_line'], 'end_line': row['end_line']} for row in (first, last)],
        'all_parsed_header_packets': delta['all_packets'],
        'vs_source_packet_counts': aggregate,
        'frequencies': {
            'eligible_packets_of_vs_source_packets': fraction(aggregate['eligible_packets'], aggregate['packets']),
            'eligible_packets_of_ungrouped_vs_source_packets': fraction(aggregate['eligible_packets'], ungrouped['packets']),
            'eligible_packets_of_all_parsed_headers': fraction(aggregate['eligible_packets'], delta['all_packets']),
            'eligible_extra_words_of_declared_vs_extra_words': fraction(aggregate['eligible_extra_words'], aggregate['extra_words']),
            'eligible_extra_words_of_declared_ungrouped_vs_extra_words': fraction(aggregate['eligible_extra_words'], ungrouped['extra_words']),
        },
        'mirrored_internal_store_words': {'available': False, 'value': None,
            'reason': 'Census records mirrored source packets, not internal store words; do not infer packets times length.'},
        'classes': classes,
        'units': {
            'packets': 'Known VS program/swizzle source headers; grouped and ungrouped are disjoint.',
            'extra_words': 'Declared incoming extra_data_length, including incomplete spans; excludes the first scalar word.',
            'eligible_extra_words': 'Declared extras of the matched eligible complete/capacity packet cohort, counted once.',
            'mirrored_packets': 'Source-packet mirror mode counts, not destination store words.',
        },
        'limitations': [
            'Percentages are matched-interval frequencies, not CPU savings or refresh/game performance.',
            'Capacity uses the logical VS offset after the original scalar header; it is not physical bounds proof.',
            'Grouped capacity is a hypothetical repeated-header predicate, not grouped batching validity.',
            'No payload is read or validated by this diagnostic; no payload equality or safe batching claim follows.',
            'Legacy code_words/swizzle_words can count mirrored internal stores and are not these source-word denominators.',
            'Absent/incomplete data is unavailable, never zero; a zero denominator has percent null.',
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path, help='Complete packet census log with bracketed snapshots')
    parser.add_argument('--session', type=int, help='Actual uint64 census session ID, not the legacy one-based index')
    parser.add_argument('--snapshots', type=int, nargs=2, metavar=('FIRST', 'LAST'), help='Exact list-counter snapshot endpoints')
    args = parser.parse_args()
    try:
        report = summarize(args.log.read_text(errors='replace'), args.session, args.snapshots)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
