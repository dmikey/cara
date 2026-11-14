#!/usr/bin/env python3
"""Attach Time Profiler to an already running Mac/iOS process, or summarize a saved trace.

This never launches, restarts, or terminates the target app. Profiling changes
execution timing; use separate uninstrumented runs for performance comparisons.
"""
import argparse
import ctypes
import hashlib
import re
import shutil
import sys
from collections import Counter, defaultdict
import json
from math import gcd
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


def summarize(xml_path, output, expected_pid=None):
    tree = ET.parse(xml_path)
    schemas = {node.get('name') for node in tree.iter('schema')}
    if schemas != {'time-profile'}:
        raise ValueError(f'Only time-profile XML is supported; found {sorted(schemas)}. '
                         'CPU counter/cycle weights must not be labeled nanoseconds.')
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

    leaf_pcs = defaultdict(Counter)
    leaf_metadata = {}
    missing_leaf_pc = Counter()
    observed_images = {}
    excluded_rows = 0
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
        thread_node = resolve(row.find('thread'))
        if expected_pid is not None:
            process = resolve(thread_node.find('process')) if thread_node is not None else None
            pid = resolve(process.find('pid')) if process is not None else None
            if pid is None or not (pid.text or '').strip().isdigit():
                raise ValueError('Cannot verify sample process PID from exported thread metadata')
            if int(pid.text) != expected_pid:
                excluded_rows += 1
                continue
        frames = []
        frame_locations = []
        for item in stack.findall('frame'):
            frame = resolve(item)
            frames.append(f"{label(frame)} [{label(frame.find('binary'))}]")
            binary = resolve(frame.find('binary'))
            if binary is not None:
                identity = {key: value for key, value in binary.attrib.items() if key not in ('id', 'ref')}
                observed_images[json.dumps(identity, sort_keys=True)] = identity
            location = {'symbol': frames[-1], 'frame': metadata(frame)}
            key = json.dumps(location, sort_keys=True)
            locations[key] = location
            frame_locations.append(key)
        if not frames:
            continue
        weight = resolve(row.find('weight'))
        if weight is not None:
            if any('cycle' in str(value).lower() for value in weight.attrib.values()):
                raise ValueError('Cycle weights cannot be interpreted as Time Profiler nanoseconds')
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
        leaf = resolve(stack.find('frame'))
        address = leaf.get('addr') if leaf is not None else None
        if address is None:
            missing_leaf_pc[thread] += 1
        else:
            try:
                pc = int(address, 0)
            except ValueError as error:
                raise ValueError(f'Unrecognized leaf PC: {address!r}') from error
            if pc < 0 or pc >= 1 << 64:
                raise ValueError('Leaf PC outside unsigned 64-bit range')
            binary = resolve(leaf.find('binary'))
            identity = {key: binary.get(key) for key in ('name', 'UUID', 'arch', 'load-addr', 'path')
                        if binary is not None and binary.get(key) is not None}
            location = {'pc': hex(pc), 'binary': identity}
            key = json.dumps(location, sort_keys=True)
            leaf_metadata[key] = dict(location, symbol=frames[0])
            leaf_pcs[thread][key] += value
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
    report = {'schema': 'time-profile', 'weight_unit': unit, 'samples': sum(samples.values()),
              'expected_pid': expected_pid, 'excluded_other_process_rows': excluded_rows,
              'trace_images': list(observed_images.values()),
              'leaf_pc_note': 'All observed leaf PCs are retained, grouped by address and image '
                              'identity, not collapsed by function name. Missing addresses are counted.',
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
                                  'inclusive_locations': ranked_locations(inclusive_locations[thread]),
                                  'missing_leaf_pc_samples': missing_leaf_pc[thread],
                                  'leaf_pcs': [dict(leaf_metadata[key], weight=weight,
                                      percent=round(100 * weight / total, 3) if total else None)
                                      for key, weight in leaf_pcs[thread].most_common()]})
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"{report['samples']} sampled stacks; weights in {unit}; summary: {output}")
    for item in report['threads'][:5]:
        print(f"\n{item['thread']}: {item['samples']} samples")
        for frame in item['self'][:8]:
            percent = f"{frame['percent']:6.2f}" if frame['percent'] is not None else '   n/a'
            print(f"  {percent}% self  {frame['symbol']}")
    return report


def local_process_identity(pid):
    """Record a live local PID without reading its arguments or environment."""
    if sys.platform != 'darwin':
        raise ValueError('--local-pid requires macOS')
    library = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
    library.proc_pidpath.argtypes = (ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32)
    library.proc_pidpath.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(4096)
    if library.proc_pidpath(pid, buffer, len(buffer)) <= 0:
        raise ValueError(f'Cannot resolve running PID {pid} executable (errno {ctypes.get_errno()})')
    started = subprocess.run(['/bin/ps', '-p', str(pid), '-o', 'lstart='], check=True,
                             capture_output=True, text=True).stdout.strip()
    if not started:
        raise ValueError(f'PID {pid} is not running')
    return {'pid': pid, 'executable': str(Path(os.fsdecode(buffer.value)).resolve()),
            'start_time_ps': started}


def binary_identity(path, env):
    path = path.resolve(strict=True)
    result = subprocess.run(['xcrun', 'dwarfdump', '--uuid', str(path)], env=env,
                            check=True, capture_output=True, text=True).stdout
    images = [{'uuid': uuid.upper(), 'architecture': arch} for uuid, arch in re.findall(
        r'UUID:\s*([0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12})\s*\(([^)]+)\)', result)]
    if not images:
        raise ValueError(f'No Mach-O UUID found in {path}')
    digest = hashlib.sha256()
    with path.open('rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            digest.update(block)
    return {'path': str(path), 'sha256': digest.hexdigest(), 'images': images}


def verify_trace_binary(report, identity):
    """Do not equate the executable currently at a path with the loaded image."""
    expected = {(image['uuid'], image['architecture']) for image in identity['images']}
    observed = {(image.get('UUID', '').upper(), image.get('arch'))
                for image in report['trace_images']}
    matches = expected & observed
    if not matches:
        raise ValueError('No trace image UUID/architecture matches the archived executable; '
                         'loaded binary identity is unverified. Preserve the raw trace.')
    return [{'uuid': uuid, 'architecture': arch} for uuid, arch in sorted(matches)]


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--pid', type=int, help='Attach to already-running device PID; never launch')
    source.add_argument('--local-pid', type=int, help='Attach to already-running Mac PID; requires --binary')
    source.add_argument('--process', help='Attach to already-running device process name; never launch')
    source.add_argument('--trace', type=Path, help='Export an existing Instruments trace')
    source.add_argument('--xml', type=Path, help='Summarize already-exported Time Profiler XML')
    parser.add_argument('--device', help='Required with --pid/--process; exact physical device UDID')
    parser.add_argument('--binary', type=Path, help='Exact executable; archived and verified against trace UUID')
    parser.add_argument('--source-fingerprint', help='Caller-supplied engine fingerprint; never inferred from a path')
    parser.add_argument('--output', type=Path, required=True, help='New output directory')
    parser.add_argument('--duration', type=int, default=20)
    parser.add_argument('--dsym', type=Path, help='Matching dSYM; raw trace remains unchanged')
    parser.add_argument('--schema', choices=['time-profile'], default='time-profile')
    parser.add_argument('--run', type=int, default=1)
    args = parser.parse_args(argv)
    if args.pid is not None and (not args.device or args.pid <= 0):
        parser.error('--pid requires --device and a positive PID')
    if args.process is not None and (not args.device or not args.process.strip()):
        parser.error('--process requires --device and a nonempty existing process name')
    if args.local_pid is not None and (args.local_pid <= 0 or args.device or not args.binary):
        parser.error('--local-pid requires a positive PID and --binary, and rejects --device')
    if args.device and args.pid is None and args.process is None:
        parser.error('--device only applies to --pid or --process')
    if not 1 <= args.duration <= 60 or args.run <= 0:
        parser.error('duration must be 1..60 seconds and run must be positive')
    return args


def main(argv=None):
    args = parse_args(argv)
    env = os.environ.copy()
    env.setdefault('DEVELOPER_DIR', '/Volumes/Fast/Applications/Xcode.app/Contents/Developer')
    before = local_process_identity(args.local_pid) if args.local_pid is not None else None
    if before and Path(before['executable']) != args.binary.resolve(strict=True):
        raise ValueError('--binary does not identify the requested PID executable')
    args.output.mkdir(parents=True, exist_ok=False)
    receipt = {'mode': 'local_pid' if before else 'device_or_saved_trace',
               'local_process_before': before, 'status': 'preparing',
               'source_fingerprint': args.source_fingerprint,
               'source_fingerprint_verification': 'caller_supplied_not_independently_verified'}
    receipt_path = args.output / 'capture-receipt.json'

    def save_receipt():
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')

    def run(*command, timeout=120):
        subprocess.run(['xcrun', 'xctrace', *map(str, command)], env=env, check=True, timeout=timeout)

    save_receipt()
    try:
        if args.binary:
            archived = args.output / 'target-binary'
            shutil.copy2(args.binary, archived)
            receipt['binary'] = binary_identity(archived, env)
            receipt['binary']['original_path'] = str(args.binary.resolve())
            save_receipt()
        trace = args.trace
        if args.local_pid is not None or args.pid is not None or args.process is not None:
            trace = args.output / 'recording.trace'
            command = ['record', '--template', 'Time Profiler']
            if args.device:
                command += ['--device', args.device]
            target = args.local_pid if args.local_pid is not None else (args.pid or args.process)
            if before and local_process_identity(args.local_pid) != before:
                raise ValueError('Local PID identity changed before attachment; no recording started')
            receipt['status'] = 'recording'
            save_receipt()
            run(*command, '--attach', target, '--time-limit', f'{args.duration}s', '--output', trace)
            if before:
                receipt['local_process_after'] = local_process_identity(args.local_pid)
                if receipt['local_process_after'] != before:
                    raise ValueError('Local PID identity changed during capture; trace is preserved')
        if args.xml:
            report = summarize(args.xml, args.output / 'cpu-stacks.json')
        else:
            if args.dsym:
                symbolicated = args.output / 'symbolicated.trace'
                run('symbolicate', '--input', trace, '--output', symbolicated, '--dsym', args.dsym)
                trace = symbolicated
            toc = args.output / 'trace-toc.xml'
            run('export', '--input', trace, '--toc', '--output', toc)
            schemas = sorted({node.get('schema') for node in ET.parse(toc).iter('table') if node.get('schema')})
            if args.schema not in schemas:
                raise ValueError(f'Schema {args.schema!r} unavailable; found {schemas}; original trace is preserved')
            xml = args.output / 'time-profile.xml'
            xpath = f'/trace-toc/run[@number="{args.run}"]/data/table[@schema="{args.schema}"]'
            run('export', '--input', trace, '--xpath', xpath, '--output', xml)
            report = summarize(xml, args.output / 'cpu-stacks.json', expected_pid=args.local_pid)
        if args.binary:
            receipt['verified_trace_images'] = verify_trace_binary(report, receipt['binary'])
        receipt['status'] = 'complete'
        save_receipt()
    except Exception as error:
        receipt['status'] = 'failed'
        receipt['error'] = str(error)
        save_receipt()
        raise


if __name__ == '__main__':
    main()
