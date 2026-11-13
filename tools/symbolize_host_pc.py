#!/usr/bin/env python3
"""Offline exact-UUID app-only wall-PC analysis. No shared-cache image guessing."""
import argparse
import collections
import json
import math
import pathlib
import re
import struct
import subprocess
import sys


def run(*args):
    p = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        raise ValueError(f'{args[0]} failed: {p.stderr.strip()}')
    return p.stdout


def uuids(path):
    result = {}
    for uuid, arch in re.findall(
            r'UUID:\s*([0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12})\s*\(([^)]+)\)',
            run('xcrun', 'dwarfdump', '--uuid', str(path))):
        uuid = uuid.upper()
        if uuid in result and result[uuid] != arch:
            raise ValueError('ambiguous UUID maps to multiple architectures')
        result[uuid] = arch
    if not result:
        raise ValueError(f'no Mach-O UUID/architecture found in {path}')
    return result


def macho_segments(path, arch):
    blob = pathlib.Path(path).read_bytes()
    offset, slice_end = 0, len(blob)
    def bounds(pos, size, end, label):
        if pos < 0 or size < 0 or pos + size > end:
            raise ValueError(f'truncated or out-of-bounds Mach-O {label}')
    bounds(0, 4, len(blob), 'magic')
    magic = struct.unpack_from('>I', blob)[0]
    if magic in (0xCAFEBABE, 0xCAFEBABF):
        bounds(0, 8, len(blob), 'fat header')
        n = struct.unpack_from('>I', blob, 4)[0]
        entry_size = 32 if magic == 0xCAFEBABF else 20
        bounds(8, n * entry_size, len(blob), 'fat architecture table')
        found = []
        for i in range(n):
            pos = 8 + i * entry_size
            cpu, subtype = struct.unpack_from('>II', blob, pos)
            fmt = '>QQ' if magic == 0xCAFEBABF else '>II'
            off, size = struct.unpack_from(fmt, blob, pos + 8)
            bounds(off, size, len(blob), 'fat slice')
            if cpu == 0x0100000C and ((subtype & 0xFFFFFF) == 2) == (arch == 'arm64e'):
                found.append((off, off + size))
        if len(found) != 1:
            raise ValueError(f'cannot select unique {arch} Mach-O slice')
        offset, slice_end = found[0]
    bounds(offset, 32, slice_end, '64-bit header')
    if struct.unpack_from('<I', blob, offset)[0] != 0xFEEDFACF:
        raise ValueError('requires little-endian 64-bit Mach-O')
    cpu, subtype, _, ncmds, sizeofcmds = struct.unpack_from('<IIIII', blob, offset + 4)
    actual_arch = 'arm64e' if (subtype & 0xFFFFFF) == 2 else 'arm64'
    if arch not in ('arm64', 'arm64e') or cpu != 0x0100000C or actual_arch != arch:
        raise ValueError('Mach-O slice architecture disagrees with UUID architecture')
    pos = offset + 32
    bounds(pos, sizeofcmds, slice_end, 'load-command region')
    end = pos + sizeofcmds
    segments = []
    for _ in range(ncmds):
        bounds(pos, 8, end, 'load-command header')
        cmd, size = struct.unpack_from('<II', blob, pos)
        if size < 8 or size % 8 or pos + size > end:
            raise ValueError('invalid Mach-O load-command size')
        if cmd == 0x19:
            if size < 72:
                raise ValueError('truncated Mach-O segment command')
            nsects = struct.unpack_from('<I', blob, pos + 64)[0]
            if 72 + nsects * 80 != size:
                raise ValueError('invalid Mach-O segment section count')
            name = blob[pos+8:pos+24].split(b'\0')[0].decode('ascii')
            vmaddr, vmsize, fileoff, filesize, _, initprot = struct.unpack_from('<QQQQii', blob, pos+24)
            if vmaddr + vmsize > 1 << 64:
                raise ValueError('Mach-O segment address overflow')
            bounds(offset + fileoff, filesize, slice_end, 'segment file range')
            segments.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize,
                                 fileoff=fileoff, filesize=filesize, initprot=initprot))
        pos += size
    if pos != end:
        raise ValueError('Mach-O command count disagrees with command region size')
    text = [s for s in segments if s['name'] == '__TEXT' and s['fileoff'] == 0]
    if len(text) != 1:
        raise ValueError('binary has no unique header-containing __TEXT segment')
    return text[0], [s for s in segments if s['initprot'] & 4 and s['vmsize']]


def number(value):
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ValueError(f'expected integer, got {value!r}')
    return int(value, 0) if isinstance(value, str) else value


def validate_report(report):
    if not isinstance(report, dict):
        raise ValueError('capture JSON must be an object')
    for key in ('images', 'samples'):
        if not isinstance(report.get(key), list) or not all(isinstance(x, dict) for x in report[key]):
            raise ValueError(f'capture {key} must be an array of objects')
    for i, image in enumerate(report['images']):
        if not isinstance(image.get('uuid'), str):
            raise ValueError(f'capture images[{i}].uuid must be a string')
        for key in ('runtime_header', 'slide'):
            number(image[key])
    for i, sample in enumerate(report['samples']):
        for key in ('pc', 'before_ns', 'after_ns', 'suspend_ns', 'natural_run_state',
                    'info_result', 'suspend_result', 'state_result', 'resume_result'):
            if key not in sample or not isinstance(sample[key], int) or isinstance(sample[key], bool):
                raise ValueError(f'capture samples[{i}].{key} must be an integer')
    for key in ('start_ns', 'end_ns', 'start_unix_ns', 'capacity', 'sampler_preflight_result',
                'info_failures', 'suspend_failures', 'state_failures', 'resume_failures', 'dropped_records'):
        if key not in report or not isinstance(report[key], int) or isinstance(report[key], bool):
            raise ValueError(f'capture {key} must be an integer')
    seconds = report.get('max_seconds')
    if isinstance(seconds, bool) or not isinstance(seconds, (int, float)) or not math.isfinite(seconds):
        raise ValueError('capture max_seconds must be a finite number')


def symbols(binary, dsym, arch, uuid, header, pcs):
    obj = pathlib.Path(binary)
    if dsym:
        root = pathlib.Path(dsym)
        files = list((root / 'Contents/Resources/DWARF').glob('*')) if root.is_dir() else [root]
        exact = [f for f in files if uuids(f).get(uuid) == arch]
        if len(exact) != 1:
            raise ValueError('dSYM must contain exactly one matching DWARF UUID and architecture')
        obj = exact[0]
    result = {}
    pcs = sorted(set(pcs))
    for i in range(0, len(pcs), 256):
        chunk = pcs[i:i+256]
        lines = run('xcrun', 'atos', '-arch', arch, '-o', str(obj), '-l', hex(header),
                    *[hex(pc) for pc in chunk]).splitlines()
        if len(lines) != len(chunk):
            raise ValueError('atos output count differs from requested addresses')
        result.update(zip(chunk, lines))
    return result, str(obj)


def analyze(args):
    report = json.loads(pathlib.Path(args.report).read_text())
    validate_report(report)
    binary_uuids = uuids(args.binary)
    matches = [(image, uuid, binary_uuids[uuid]) for image in report.get('images', [])
               if (uuid := image.get('uuid', '').upper()) in binary_uuids]
    if len(matches) != 1:
        raise ValueError('capture must contain exactly one image matching an exact binary UUID')
    image, uuid, arch = matches[0]
    if arch not in ('arm64', 'arm64e'):
        raise ValueError(f'unsupported architecture {arch}')
    if args.expected_uuid and uuid != args.expected_uuid.upper():
        raise ValueError('binary/capture UUID differs from --expected-uuid')
    text, executable = macho_segments(args.binary, arch)
    header = number(image['runtime_header'])
    slide = header - text['vmaddr']
    if slide != number(image['slide']):
        raise ValueError('runtime header, __TEXT vmaddr and reported slide disagree')
    ranges = [dict(name=s['name'], start=s['vmaddr'] + slide,
                   end=s['vmaddr'] + slide + s['vmsize']) for s in executable]
    if not ranges:
        raise ValueError('binary has no executable segments')
    samples = report.get('samples', [])
    warnings = []
    checks = {}
    checks['capacity_respected'] = len(samples) <= number(report.get('capacity', 0)) <= 6000
    checks['max_seconds_bounded'] = 0 < report.get('max_seconds', 0) <= 60
    checks['capacity_stop_full'] = report.get('stop_reason') != 'capacity' or len(samples) == report.get('capacity')
    checks['preflight_success'] = report.get('sampler_preflight_result') == 0
    checks['clock_bounds_present'] = 0 < report.get('start_ns', 0) <= report.get('end_ns', 0)
    checks['wall_anchor_present'] = report.get('start_unix_ns', 0) > 0
    checks['known_stop_reason'] = report.get('stop_reason') in ('capacity', 'time_limit', 'stopped')
    checks['resume_balanced'] = all(s.get('resume_result') == 0 for s in samples if s.get('suspend_result') == 0)
    checks['dropped_zero'] = report.get('dropped_records') == 0
    computed = {
        'info_failures': sum(s.get('info_result') != 0 for s in samples),
        'suspend_failures': sum(s.get('suspend_result') != 0 for s in samples),
        'state_failures': sum(s.get('suspend_result') == 0 and s.get('state_result') != 0 for s in samples),
        'resume_failures': sum(s.get('suspend_result') == 0 and s.get('resume_result') != 0 for s in samples),
    }
    checks['failure_counters_agree'] = all(report.get(k) == v for k, v in computed.items())
    checks['mach_calls_successful'] = all(v == 0 for v in computed.values())
    valid = []
    invalid = []
    prior = 0
    for i, s in enumerate(samples):
        before, after, suspended = (number(s.get(k, 0)) for k in ('before_ns', 'after_ns', 'suspend_ns'))
        clock_ok = (report.get('start_ns', 0) <= before <= after <= report.get('end_ns', 0)
                    and before >= prior and 0 <= suspended <= after - before)
        prior = before
        success = all(s.get(k) == 0 for k in ('suspend_result', 'state_result', 'resume_result'))
        if clock_ok and success and number(s.get('pc', 0)) > 0:
            valid.append(s)
        else:
            invalid.append(dict(index=i, clock_valid=clock_ok, raw_record=s))
    checks['all_sample_clocks_valid'] = all(x['clock_valid'] for x in invalid)
    # Clock failures can occur only in invalid records; valid records passed above.
    checks['all_raw_pcs_valid'] = len(valid) == len(samples)
    app_pcs = [number(s['pc']) for s in valid if any(r['start'] <= number(s['pc']) < r['end'] for r in ranges)]
    names, symbol_file = symbols(args.binary, args.dsym, arch, uuid, header, app_pcs)
    def population(records, label):
        app = collections.Counter()
        unknown = collections.Counter()
        app_addresses = collections.Counter()
        for s in records:
            pc = number(s['pc'])
            if pc in names:
                name = names[pc]
                # atos may return only an address when debug symbols are missing.
                app[name] += 1
                app_addresses[(pc, name)] += 1
            else:
                unknown[pc] += 1
        denominator = len(records)
        rows = [dict(symbol=name, samples=n, percent_of_population=100*n/denominator)
                for name, n in app.most_common()]
        return dict(label=label, valid_samples=denominator, app_samples=sum(app.values()),
                    non_app_or_unknown_samples=sum(unknown.values()), app_symbols=rows,
                    app_addresses=[dict(pc=hex(pc), symbol=name, samples=n) for (pc,name),n in app_addresses.most_common()],
                    non_app_or_unknown_addresses=[dict(pc=hex(pc), samples=n) for pc,n in unknown.most_common()])
    elapsed = (report.get('end_ns', 0) - report.get('start_ns', 0))/1e9
    suspension = [number(s.get('suspend_ns', 0)) for s in samples if s.get('suspend_result') == 0]
    for key, passed in checks.items():
        if not passed:
            warnings.append(key)
    if report.get('stop_reason') == 'capacity':
        warnings.append('capture hit fixed capacity; later residency is not represented')
    result = dict(kind='wall_time_pc_residency_not_cpu_percent',
        interpretation='Natural RUNNING means runnable; neither population measures on-CPU shares. Non-app addresses remain unresolved; no nearest-image-header attribution.',
        source_report=str(pathlib.Path(args.report).resolve()), source_fingerprint=report.get('source_fingerprint'),
        app_image=image, captured_images=report.get('images', []), exact_uuid=uuid, binary=str(pathlib.Path(args.binary).resolve()), symbol_file=symbol_file,
        executable_runtime_ranges=ranges, capture_configuration={k:report.get(k) for k in ('capacity','max_seconds','nominal_hz','jitter_interval_us','start_frame','last_observed_frame','requested_start_frame','requested_end_frame','start_ns','end_ns','start_unix_ns')}, quality=dict(checks=checks, warnings=warnings,
            sample_count=len(samples), valid_count=len(valid), invalid_records=invalid,
            elapsed_seconds=elapsed, actual_samples_per_second=len(samples)/elapsed if elapsed > 0 else None,
            natural_run_state_counts=dict(collections.Counter(str(s.get('natural_run_state', 'unknown')) for s in valid)),
            failure_counters=computed, stop_reason=report.get('stop_reason'),
            total_suspension_ns=sum(suspension), max_suspension_ns=max(suspension, default=0)),
        populations=[population(valid, 'all valid wall-time PCs'),
                     population([s for s in valid if s.get('natural_run_state') == 1], 'natural RUNNING subset (runnable, not on-CPU)')])
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('report')
    p.add_argument('--binary', required=True)
    p.add_argument('--dsym')
    p.add_argument('--expected-uuid')
    p.add_argument('--output', required=True)
    p.add_argument('--top', type=int, default=20)
    args = p.parse_args()
    try:
        result = analyze(args)
        pathlib.Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
        print(f"Exact app UUID {result['exact_uuid']}; {result['quality']['valid_count']}/{result['quality']['sample_count']} valid wall PCs")
        print('Quality warnings:', ', '.join(result['quality']['warnings']) or 'none')
        for pop in result['populations']:
            print(f"\n{pop['label']}: {pop['valid_samples']} samples; app={pop['app_samples']}; unresolved/non-app={pop['non_app_or_unknown_samples']}")
            for row in pop['app_symbols'][:args.top]:
                print(f"{row['samples']:5} {row['percent_of_population']:6.2f}%  {row['symbol']}")
        print('\nWrote', args.output)
    except (ValueError, OSError, KeyError, TypeError, struct.error) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
