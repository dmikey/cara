#!/usr/bin/env python3
"""Summarize raw, instrumented guest-PC thread CPU windows; never extrapolate."""
import argparse
import json
import math
from pathlib import Path


KIND = 'thread_cpu_by_block_start_disjoint_ranges'
RESERVED = {'other_guest', 'svc_external'}
EPISODE_COUNTS = ('started', 'completed', 'aborted', 'reentries',
                  'partial_sampled_episodes', 'sampled_aborted_episodes')
EPISODE_TIMES = ('sampled_completed_cpu_ns', 'sampled_partial_cpu_ns',
                 'sampled_aborted_cpu_ns', 'active_sampled_cpu_ns')


def integer(value, label, maximum=None):
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f'{label} must be a nonnegative integer')
    if maximum is not None and value > maximum:
        raise ValueError(f'{label} exceeds {maximum}')
    return value


def boolean(value, label):
    if not isinstance(value, bool):
        raise ValueError(f'{label} must be boolean')
    return value


def per_refresh(cpu_ns, count, valid):
    return cpu_ns / count if valid and count else None


def summarize_episode(episode, bucket_cpu_ns):
    result = {key: integer(episode[key], f'episode {key}')
              for key in EPISODE_COUNTS + EPISODE_TIMES}
    result['active'] = boolean(episode['active'], 'episode active')
    result['active_partial'] = boolean(episode['active_partial'], 'episode active_partial')
    if sum(result[key] for key in EPISODE_TIMES) != bucket_cpu_ns:
        raise ValueError('episode CPU subtotals do not reconcile with episode bucket')
    if not result['active'] and (result['active_partial'] or result['active_sampled_cpu_ns']):
        raise ValueError('inactive episode has active CPU or partial state')
    if result['sampled_aborted_episodes'] > result['aborted']:
        raise ValueError('sampled aborted episodes exceed all aborted episodes')
    if result['sampled_aborted_cpu_ns'] and not result['sampled_aborted_episodes']:
        raise ValueError('aborted CPU without a sampled aborted episode')
    if result['sampled_completed_cpu_ns'] and not result['completed']:
        raise ValueError('completed CPU without a completed episode')
    if result['sampled_partial_cpu_ns'] and not result['partial_sampled_episodes']:
        raise ValueError('partial CPU without a partial sampled episode')
    result['sampled_span_cpu_ns'] = bucket_cpu_ns
    result['all_sampled_cpu_from_fully_completed_episodes'] = not (
        result['sampled_partial_cpu_ns'] or result['sampled_aborted_episodes'] or
        result['active_sampled_cpu_ns'])
    # completed/started include unsampled fences, so no per-episode average.
    return result


def summarize_core(core):
    core_id = integer(core.get('core'), 'core')
    profile = core.get('guest_pc_cpu')
    if profile is None:
        return {'core': core_id, 'status': 'unavailable',
                'issues': ['guest_pc_cpu field absent'], 'valid_for_per_refresh': False}
    enabled = boolean(profile.get('enabled'), 'enabled')
    if not enabled:
        error = profile.get('config_error', '')
        if not isinstance(error, str):
            raise ValueError('config_error must be a string')
        return {'core': core_id, 'status': 'invalid_config' if error else 'disabled',
                'issues': [f'config_error: {error}'] if error else [],
                'valid_for_per_refresh': False}
    if profile.get('kind') != KIND:
        raise ValueError(f'core {core_id}: unknown guest_pc_cpu kind')
    mode = profile.get('mode', 'disjoint')
    if mode not in ('disjoint', 'episode'):
        raise ValueError('unknown guest_pc_cpu mode')
    fences = None
    if mode == 'episode':
        fences = {key: integer(profile[key], key, (1 << 32) - 1)
                  for key in ('episode_start_pc', 'episode_end_pc')}
        if fences['episode_start_pc'] == fences['episode_end_pc']:
            raise ValueError('episode start/end fences must differ')
    error = profile.get('config_error')
    if not isinstance(error, str):
        raise ValueError('config_error must be a string')
    issues = [f'config_error: {error}'] if error else []
    ranges = profile['ranges']
    names, bounds = set(RESERVED), []
    for region in ranges:
        name = region['name']
        if not isinstance(name, str) or not name or name in names:
            raise ValueError('range names must be unique and not reserved')
        names.add(name)
        start = integer(region['start'], 'range start', (1 << 32) - 1)
        end = integer(region['end'], 'range end', 1 << 32)
        if start >= end:
            raise ValueError('range must be nonempty and half open')
        bounds.append((start, end))
    bounds.sort()
    if any(a[1] > b[0] for a, b in zip(bounds, bounds[1:])):
        raise ValueError('PC ranges must be disjoint')
    if mode == 'episode':
        if not any(start <= fences['episode_start_pc'] < end for start, end in bounds):
            raise ValueError('episode start fence outside allowlist')
        # In episode mode range names describe the allowlist, not CPU buckets.
        names = RESERVED | {'episode'}
    counters = {key: integer(profile[key], key) for key in (
        'frame_resets', 'window_frames', 'period_frames', 'window_count', 'start_frame',
        'sampled_refreshes', 'cpu_ns', 'clock_reads', 'clock_failures',
        'unassigned_intervals', 'calibration_reads', 'calibration_failures',
        'thread_table_overflow_entries',
        'straddling_blocks')}
    base = profile['base_frame']
    if base is not None:
        integer(base, 'base_frame', (1 << 32) - 1)
    ids = profile['sampled_frame_ids']
    if not isinstance(ids, list):
        raise ValueError('sampled_frame_ids must be an array')
    for frame in ids:
        integer(frame, 'sampled_frame_id', (1 << 32) - 1)
    if ids != sorted(set(ids)):
        raise ValueError('sampled_frame_ids must be ordered and unique')
    if len(ids) != counters['sampled_refreshes']:
        raise ValueError('sampled_refreshes must equal sampled_frame_ids length')
    if not ids:
        issues.append('no sampled refreshes')
    if base is None:
        issues.append('base frame unavailable')
    if not counters['window_frames'] or counters['period_frames'] < counters['window_frames']:
        issues.append('invalid configured window/period')
    if not counters['window_count']:
        issues.append('no requested windows')
    windows = profile['windows']
    if not isinstance(windows, list):
        raise ValueError('windows must be an array')
    observed_ids, requested_ids, window_indexes = set(), set(), set()
    for window in windows:
        index = integer(window['index'], 'window index')
        if index >= counters['window_count']:
            raise ValueError('window index outside configured window_count')
        if index in window_indexes:
            raise ValueError('duplicate window index')
        window_indexes.add(index)
        start = integer(window['start_frame'], 'window start', (1 << 32) - 1)
        end = integer(window['end_frame'], 'window end', 1 << 32)
        observed = integer(window['observed_frames'], 'observed_frames')
        closed = boolean(window['closed'], 'window closed')
        complete = boolean(window['complete'], 'window complete')
        if start >= end:
            raise ValueError('window must have nonempty half-open bounds')
        if end - start != counters['window_frames']:
            raise ValueError('window length differs from configured window_frames')
        if base is not None and start != (base + counters['start_frame'] +
                                          index * counters['period_frames']):
            raise ValueError('window start differs from base/configured offset and period')
        # Use only actual IDs; avoid materializing an untrusted huge range.
        contained = {frame for frame in ids if start <= frame < end}
        if observed != len(contained):
            raise ValueError('observed_frames differs from IDs inside window')
        if observed_ids & contained:
            raise ValueError('sampled refresh belongs to overlapping windows')
        observed_ids.update(contained)
        if any(start < b and a < end for a, b in requested_ids):
            raise ValueError('requested windows overlap')
        requested_ids.add((start, end))
        if not closed or not complete or observed != end - start:
            issues.append(f'window {index} incomplete or open')
    if observed_ids != set(ids):
        raise ValueError('sampled frame IDs outside listed windows')
    if len(windows) > counters['window_count']:
        raise ValueError('started windows exceed configured window_count')
    if len(windows) < counters['window_count']:
        issues.append('configured windows not all represented')
    for key in ('frame_resets', 'clock_failures', 'calibration_failures', 'unassigned_intervals',
                'thread_table_overflow_entries', 'straddling_blocks'):
        if counters[key]:
            issues.append(f'{key}={counters[key]}')
    capacity_exhausted = boolean(profile['sample_capacity_exhausted'],
                                 'sample_capacity_exhausted')
    if capacity_exhausted:
        issues.append('sample capacity exhausted')
    coverage = boolean(profile['coverage_complete'], 'coverage_complete')
    if not coverage:
        issues.append('profiler reports incomplete coverage')
    threads, seen_threads, totals = [], set(), {name: 0 for name in names}
    for thread in profile['threads']:
        thread_id = thread['thread_id']
        if thread_id is not None:
            integer(thread_id, 'thread_id', (1 << 32) - 1)
        overflow = boolean(thread['overflow'], 'thread overflow')
        identity = (thread_id, overflow)
        if identity in seen_threads:
            raise ValueError('duplicate thread bucket')
        seen_threads.add(identity)
        if overflow:
            issues.append('overflow thread bucket present')
        cpu_ns = integer(thread['cpu_ns'], 'thread cpu_ns')
        buckets, seen, subtotal = [], set(), 0
        for bucket in thread['buckets']:
            name = bucket['name']
            if name not in names or name in seen:
                raise ValueError('unknown or duplicate CPU bucket')
            seen.add(name)
            value = integer(bucket['cpu_ns'], 'bucket cpu_ns')
            blocks = integer(bucket['blocks'], 'bucket blocks')
            transitions = integer(bucket['transitions'], 'bucket transitions')
            subtotal += value
            totals[name] += value
            buckets.append({'name': name, 'cpu_ns': value, 'blocks': blocks,
                            'transitions': transitions})
        if subtotal != cpu_ns:
            raise ValueError('thread CPU does not reconcile with bucket sum')
        if seen != names:
            raise ValueError('thread must include every configured and reserved bucket')
        result = {'thread_id': thread_id, 'overflow': overflow,
                  'cpu_ns': cpu_ns, 'buckets': buckets}
        if mode == 'episode':
            episode = summarize_episode(thread['episode'], next(
                bucket['cpu_ns'] for bucket in buckets if bucket['name'] == 'episode'))
            if episode['sampled_aborted_episodes']:
                issues.append(f'thread {thread_id}: sampled episode aborted')
            result['episode'] = episode
        threads.append(result)
    if sum(thread['cpu_ns'] for thread in threads) != counters['cpu_ns']:
        raise ValueError('core CPU does not reconcile with thread sum')
    calibration_cost = profile['clock_ns_per_read']
    if (isinstance(calibration_cost, bool) or not isinstance(calibration_cost, (int, float))
            or not math.isfinite(calibration_cost) or calibration_cost < 0):
        raise ValueError('clock_ns_per_read must be finite and nonnegative')
    calibration_available = bool(counters['calibration_reads'] > 1 and calibration_cost > 0
                                 and not counters['calibration_failures'])
    budget = counters['clock_reads'] * calibration_cost if calibration_available else None
    if budget is not None and not math.isfinite(budget):
        raise ValueError('nonfinite clock overhead budget')
    valid = not issues
    count = len(ids)
    for thread in threads:
        thread['cpu_ns_per_sampled_refresh'] = per_refresh(thread['cpu_ns'], count, valid)
        for bucket in thread['buckets']:
            bucket['cpu_ns_per_sampled_refresh'] = per_refresh(bucket['cpu_ns'], count, valid)
        if mode == 'episode':
            episode = thread['episode']
            episode['cpu_ns_per_sampled_refresh'] = {
                key: per_refresh(episode[key], count, valid)
                for key in EPISODE_TIMES + ('sampled_span_cpu_ns',)}
    episode_totals = None
    if mode == 'episode':
        episode_totals = {
            key: sum(thread['episode'][key] for thread in threads)
            for key in EPISODE_COUNTS + EPISODE_TIMES + ('sampled_span_cpu_ns',)}
        episode_totals['active_threads'] = sum(thread['episode']['active'] for thread in threads)
        episode_totals['active_partial_threads'] = sum(
            thread['episode']['active_partial'] for thread in threads)
        episode_totals['cpu_ns_per_sampled_refresh'] = {
            key: per_refresh(episode_totals[key], count, valid)
            for key in EPISODE_TIMES + ('sampled_span_cpu_ns',)}
    return {'core': core_id, 'status': 'complete' if valid else 'incomplete',
            'mode': mode, 'episode_fences': fences,
            'episode': episode_totals,
            'issues': issues, 'valid_for_per_refresh': valid,
            'base_frame': base, 'sampled_frame_ids': ids, 'sampled_refreshes': count,
            'requested_windows_not_started': counters['window_count'] - len(windows),
            'reported_coverage_complete': coverage,
            'sample_capacity_exhausted': capacity_exhausted,
            'windows': windows, 'cpu_ns': counters['cpu_ns'],
            'cpu_ns_per_sampled_refresh': per_refresh(counters['cpu_ns'], count, valid),
            'buckets': [{'name': name, 'cpu_ns': value,
                         'cpu_ns_per_sampled_refresh': per_refresh(value, count, valid)}
                        for name, value in sorted(totals.items())],
            'threads': threads, 'diagnostics': counters,
            'clock_read_budget': {'calibration_available': calibration_available,
                                 'calibration_reads': counters['calibration_reads'],
                                 'clock_reads': counters['clock_reads'],
                                 'clock_ns_per_read': calibration_cost if calibration_available else None,
                                 'estimated_cpu_ns': budget,
                                 'estimated_cpu_ns_per_sampled_refresh':
                                     per_refresh(budget, count, valid) if budget is not None else None,
                                 'subtracted_from_cpu_ns': False}}


def summarize(report):
    cores = [summarize_core(core) for core in report['cores']]
    if len({core['core'] for core in cores}) != len(cores):
        raise ValueError('duplicate core IDs')
    measured = [core for core in cores if 'cpu_ns' in core]
    ids = measured[0]['sampled_frame_ids'] if measured else []
    aligned = bool(measured) and all(core['valid_for_per_refresh'] and
                                  core['sampled_frame_ids'] == ids for core in measured)
    total = sum(core['cpu_ns'] for core in measured) if measured else None
    return {'kind': KIND, 'units': {'cpu_ns': 'raw instrumented thread CPU nanoseconds'},
            'notes': ['no extrapolation to unmeasured frames',
                      'clock-read budget is approximate, separate, and not subtracted',
                      'global conversion helper buckets include all callers; not loop-inclusive',
                      'episode spans include owned helper/SVC CPU; partial spans are not completed episodes',
                      'episode fence counts include unsampled execution; no per-episode average inferred',
                      'PC buckets include dispatch/memory and any attributed external SVC cost',
                      'host work outside profiled Run intervals is excluded'],
            'cores': cores,
            'aggregate': {'cpu_ns': total, 'measured_cores': [core['core'] for core in measured],
                          'same_complete_sampled_frames': aligned,
                          'sampled_refreshes': len(ids) if aligned else None,
                          'cpu_ns_per_sampled_refresh': per_refresh(total, len(ids), aligned)}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path, help='fastinterp-blocks.json')
    args = parser.parse_args()
    try:
        result = summarize(json.loads(args.report.read_text()))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
