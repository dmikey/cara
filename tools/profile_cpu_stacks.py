#!/usr/bin/env python3
"""Attach Time Profiler to an already running iOS process, or summarize a saved trace.

This never launches, restarts, or terminates the target app. Profiling changes
execution timing; use separate uninstrumented runs for performance comparisons.
"""
import argparse
from collections import Counter, defaultdict
import json
from math import gcd
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


def summarize(xml_path, output):
    tree = ET.parse(xml_path)
    definitions = {(node.tag, node.attrib['id']): node for node in tree.iter() if 'id' in node.attrib}

    def resolve(node):
        if node is not None and 'ref' in node.attrib:
            return definitions.get((node.tag, node.attrib['ref']), node)
        return node

    def label(node):
        node = resolve(node)
        if node is None:
            return 'unknown'
        return node.get('name') or node.get('fmt') or (node.text or '').strip() or 'unknown'

    totals = Counter()
    own = defaultdict(Counter)
    inclusive = defaultdict(Counter)
    own_locations = defaultdict(Counter)
    inclusive_locations = defaultdict(Counter)
    locations = {}
    samples = Counter()
    units = Counter()
    weights = Counter()

    def metadata(node, seen=()):
        """Retain exporter-specific address/UUID/load-address fields without guessing units."""
        node = resolve(node)
        if node is None:
            return None
        result = {'attributes': dict(node.attrib)}
        if id(node) in seen:
            result['recursive_reference'] = True
            return result
        if node.text and node.text.strip():
            result['text'] = node.text.strip()
        children = [metadata(child, (*seen, id(node))) | {'tag': child.tag} for child in node]
        if children:
            result['children'] = children
        return result

    for row in tree.iter('row'):
        stack_node = row.find('backtrace')
        if stack_node is None:
            stack_node = row.find('tagged-backtrace')
        stack = resolve(stack_node)
        if stack is None:
            continue
        frames = []
        frame_locations = []
        for item in stack.findall('frame'):
            frame = resolve(item)
            frames.append(f"{label(frame)} [{label(frame.find('binary'))}]")
            location = {'symbol': frames[-1], 'frame': metadata(frame)}
            key = json.dumps(location, sort_keys=True)
            locations[key] = location
            frame_locations.append(key)
        if not frames:
            continue
        weight = resolve(row.find('weight'))
        if weight is not None:
            try:
                value = int(weight.text or '0')
            except ValueError:
                raise ValueError('Unrecognized Time Profiler weight; inspect exported XML')
            units['nanoseconds'] += 1
        else:
            value = 1
            units['samples'] += 1
        if value < 0:
            raise ValueError('Negative Time Profiler weight')
        weights[value] += 1
        thread = label(row.find('thread'))
        totals[thread] += value
        samples[thread] += 1
        own[thread][frames[0]] += value
        own_locations[thread][frame_locations[0]] += value
        for frame in set(frames):
            inclusive[thread][frame] += value
        for location in set(frame_locations):
            inclusive_locations[thread][location] += value
    if not totals:
        raise ValueError('No backtrace rows found; inspect trace-toc.xml and choose --schema')
    if len(units) != 1:
        raise ValueError('Mixed weighted and unweighted rows cannot be aggregated safely')
    unit = next(iter(units))
    report = {'weight_unit': unit, 'samples': sum(samples.values()),
              'weight_distribution': [{'weight': weight, 'rows': count}
                                      for weight, count in sorted(weights.items())],
              'constant_sample_weight': next(iter(weights)) if len(weights) == 1 else None,
              'observed_weight_gcd': gcd(*weights),
              'weight_note': 'Weights are exported row weights, not an inferred sampling period.',
              'inclusive_semantics': 'Each symbol or location counts once per sampled stack; '
                                     'inclusive percentages can sum above 100.',
              'location_metadata': 'Resolved XML frame/binary attributes and children are '
                                   'preserved without converting address units. Missing fields '
                                   'remain absent. Raw trace/XML is authoritative.',
              'locations': list(locations.values()),
              'threads': []}
    for thread, total in totals.most_common():
        def ranked(counter):
            return [{'symbol': symbol, 'weight': weight,
                     'percent': round(100 * weight / total, 3) if total else None}
                    for symbol, weight in counter.most_common(40)]
        def ranked_locations(counter):
            return [dict(locations[key], weight=weight,
                         percent=round(100 * weight / total, 3) if total else None)
                    for key, weight in counter.most_common(80)]
        report['threads'].append({'thread': thread, 'samples': samples[thread], 'weight': total,
                                  'self': ranked(own[thread]), 'inclusive': ranked(inclusive[thread]),
                                  'self_locations': ranked_locations(own_locations[thread]),
                                  'inclusive_locations': ranked_locations(inclusive_locations[thread])})
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"{report['samples']} sampled stacks; weights in {unit}; summary: {output}")
    for item in report['threads'][:5]:
        print(f"\n{item['thread']}: {item['samples']} samples")
        for frame in item['self'][:8]:
            percent = f"{frame['percent']:6.2f}" if frame['percent'] is not None else '   n/a'
            print(f"  {percent}% self  {frame['symbol']}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--pid', type=int, help='Attach to this already-running PID; never launch')
    source.add_argument('--process', help='Attach to this already-running process name; never launch')
    source.add_argument('--trace', type=Path, help='Export an existing Instruments trace')
    source.add_argument('--xml', type=Path, help='Summarize already-exported Time Profiler XML')
    parser.add_argument('--device', help='Required with --pid/--process; exact physical device UDID')
    parser.add_argument('--output', type=Path, required=True, help='New output directory')
    parser.add_argument('--duration', type=int, default=20)
    parser.add_argument('--dsym', type=Path, help='Matching dSYM; raw trace remains unchanged')
    parser.add_argument('--schema', default='time-profile')
    parser.add_argument('--run', type=int, default=1)
    args = parser.parse_args()
    if args.pid is not None and (not args.device or args.pid <= 0):
        parser.error('--pid requires --device and a positive PID')
    if args.process is not None and (not args.device or not args.process.strip()):
        parser.error('--process requires --device and a nonempty existing process name')
    if not 1 <= args.duration <= 60 or args.run <= 0:
        parser.error('duration must be 1..60 seconds and run must be positive')
    if not args.schema.replace('-', '').replace('_', '').isalnum():
        parser.error('invalid schema')
    args.output.mkdir(parents=True, exist_ok=False)
    if args.xml:
        summarize(args.xml, args.output / 'cpu-stacks.json')
        return
    env = os.environ.copy()
    env.setdefault('DEVELOPER_DIR', '/Volumes/Fast/Applications/Xcode.app/Contents/Developer')

    def run(*command, timeout=120):
        subprocess.run(['xcrun', 'xctrace', *map(str, command)], env=env, check=True, timeout=timeout)

    trace = args.trace
    if args.pid is not None or args.process is not None:
        trace = args.output / 'recording.trace'
        run('record', '--template', 'Time Profiler', '--device', args.device,
            '--attach', args.pid if args.pid is not None else args.process,
            '--time-limit', f'{args.duration}s', '--output', trace)
    if args.dsym:
        symbolicated = args.output / 'symbolicated.trace'
        run('symbolicate', '--input', trace, '--output', symbolicated, '--dsym', args.dsym)
        trace = symbolicated
    toc = args.output / 'trace-toc.xml'
    run('export', '--input', trace, '--toc', '--output', toc)
    schemas = sorted({node.get('schema') for node in ET.parse(toc).iter('table') if node.get('schema')})
    if args.schema not in schemas:
        raise ValueError(f"Schema {args.schema!r} unavailable; found {schemas}; original trace is preserved")
    xml = args.output / 'time-profile.xml'
    xpath = f'/trace-toc/run[@number="{args.run}"]/data/table[@schema="{args.schema}"]'
    run('export', '--input', trace, '--xpath', xpath, '--output', xml)
    summarize(xml, args.output / 'cpu-stacks.json')


if __name__ == '__main__':
    main()
