#!/usr/bin/env python3
"""Compare fixed 50/100% guest-clock diagnostics; never select an optimization winner."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import tempfile

COUNTS = ('gpu_vertex_draws', 'cpu_vertex_draws', 'gpu_vertices', 'gpu_input_records',
          'pica_command_lists', 'pica_draw_requests', 'top_buffer_swaps', 'stereo_top_buffer_swaps',
          'pica_post_irq_submissions', 'pica_post_irq_writes', 'pica_post_irq_draw_requests',
          'pica_post_irq_jumps', 'pica_first_irq_remaining_words',
          'experimental_ppf_scheduled', 'experimental_ppf_delivered', 'experimental_ppf_modeled_ns', 'experimental_ppf_queued_ticks',
          'experimental_p3d_scheduled', 'experimental_p3d_delivered', 'experimental_p3d_retired',
          'experimental_p3d_modeled_ns', 'experimental_p3d_queued_ticks',
          'vertex_gl_subimage_calls', 'vertex_gl_image_calls', 'vertex_upload_bytes',
          'vertex_index_upload_bytes', 'vertex_upload_reused_bytes',
          'draw_texture_acquire_calls', 'draw_framebuffer_acquire_calls',
          'draw_dirty_invalidate_calls', 'cpu_surface_invalidations', 'shader_calls')
DURATIONS = ('mean_ms', 'emulation_thread_cpu_ms_per_frame', 'process_cpu_ms_per_frame',
             'guest_cpu_ms_per_frame', 'draw_ms_per_frame', 'cpu_shader_ms_per_frame',
             'draw_submit_ms_per_frame', 'vertex_upload_ms_per_frame',
             'vertex_subimage_upload_ms_per_frame', 'vertex_reuse_check_ms_per_frame',
             'draw_texture_acquire_ms_per_frame', 'draw_dirty_invalidate_ms_per_frame')
DERIVED = {'name', 'guest_cpu_percent', 'fingerprint', 'means', 'audio', 'fastinterp_cache',
           'alu_resident_entry_weighted_slots', 'alu_pair_resident_entry_weighted_dispatches',
           'renderer_rgba_sha256'}
BOOKKEEPING = {'draw_texture_acquire_calls', 'draw_framebuffer_acquire_calls',
               'draw_dirty_invalidate_calls', 'draw_texture_acquire_ms_per_frame',
               'draw_dirty_invalidate_ms_per_frame'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value, context, integer=False):
    require(type(value) in (int, float) and math.isfinite(value) and value >= 0,
            context + ': expected finite nonnegative number')
    require(not integer or type(value) is int, context + ': expected integer')
    return value


def finite_tree(value, context):
    if isinstance(value, dict):
        for key, item in value.items():
            finite_tree(item, context + '/' + key)
    elif isinstance(value, list):
        for item in value:
            finite_tree(item, context)
    elif type(value) in (int, float):
        number(value, context)


def read(path):
    return json.loads(path.read_text())


def presentation_metadata(value):
    factor = value.get('factor_3d_percent')
    if factor is not None:
        require(number(factor, 'factor_3d_percent', True) <= 100, 'Invalid 3D slider percentage')
    mode = value.get('presentation_mode')
    require(mode is None or isinstance(mode, str), 'Invalid presentation mode')
    return {'factor_3d_percent': factor, 'presentation_mode': mode}


def compare(directory, warmup=300, end_frame=None):
    manifest = read(directory / 'comparison.json')
    checkpoint = (directory / 'profile-comparison.carastate').read_bytes()
    require(len(checkpoint) >= 112 and checkpoint[:8] == b'CARAST01', 'Invalid checkpoint header')
    require(hashlib.sha256(checkpoint).hexdigest() == manifest['checkpoint_sha256'],
            'Checkpoint hash differs from comparison.json')
    title = struct.unpack_from('<Q', checkpoint, 16)[0]
    fingerprint = checkpoint[48:112].decode('ascii')
    require(type(warmup) is int and warmup >= 0, 'Invalid warmup boundary')
    runs, intervals, modes, configs, names = [], [], [], [], set()
    for config in manifest['runs']:
        name = config['name']
        require(isinstance(name, str) and name and
                all(c.isascii() and (c.isalnum() or c in '_-') for c in name) and name not in names,
                'Invalid or duplicate run name')
        names.add(name)
        report = read(directory / (name + '-frame-timing.json'))
        status = read(directory / (name + '-benchmark-status.json'))
        audio = read(directory / (name + '-audio-status.json'))
        clock = config.get('guest_cpu_percent', 100)
        require(type(clock) is int and clock in (50, 100), name + ': clock must be 50 or 100')
        require(config['fingerprint'] == report['engine_build_fingerprint'] == fingerprint,
                name + ': checkpoint/build mismatch')
        require(report['title_id'] == status['title_id'] == title, name + ': title mismatch')
        require(report['cpu_backend'] == status['cpu_backend'] == 'fastinterp' and
                status['jit'] is False and status['software_renderer'] is False and
                status['gpu_backend'] == 'native_gles3', name + ': incorrect CPU/render backend')
        require(status['result_code'] == 0 and status['frames'] == status['target_frames'] and
                number(status['frames'], name + '/frames', True) > 0,
                name + ': incomplete benchmark')
        require(number(status.get('paused_seconds', 0), name + '/paused_seconds') <= .001,
                name + ': interrupted/paused benchmark')
        require(type(status['guest_cpu_percent']) is int and status['guest_cpu_percent'] == clock and
                status['adaptive_guest_cpu'] is False, name + ': incorrect/adaptive clock')
        require(audio.get('muted') is False and audio.get('queue_running') is True and
                audio.get('last_error') == 0 and
                number(audio.get('nonzero_frames', 0), name + '/audio_frames') > 0,
                name + ': muted/stopped/silent/erroring audio')
        windows = report['windows']
        require(windows, name + ': empty windows')
        previous = None
        for w in windows:
            finite_tree(w, name + '/window')
            end = number(w['end_frame'], name + '/end_frame', True)
            frames = number(w['frames'], name + '/frames', True)
            require(frames > 0 and end >= frames, name + ': invalid frame range')
            require(previous is None or end - frames == previous, name + ': noncontiguous windows')
            previous = end
            require(type(w['guest_cpu_percent']) is int and w['guest_cpu_percent'] == clock,
                    name + ': changing guest clock')
            require(not w.get('packed_validation_failures', 0) and
                    not w.get('texture_lookup_verify_mismatches', 0), name + ': rendering validation failed')
        finish = status['frames'] if end_frame is None else end_frame
        require(type(finish) is int and warmup < finish <= status['frames'], 'Invalid end boundary')
        selected = [w for w in windows if warmup < w['end_frame'] <= finish]
        require(selected and selected[0]['end_frame'] - selected[0]['frames'] == warmup and
                selected[-1]['end_frame'] == finish, name + ': boundaries cut/miss windows')
        interval = [(w['end_frame'] - w['frames'], w['end_frame']) for w in selected]
        intervals.append((warmup, finish))
        frames = sum(w['frames'] for w in selected)
        metrics = {}
        for key in COUNTS + DURATIONS:
            available = all(key in w for w in selected)
            if key in BOOKKEEPING and not config.get('render_bookkeeping_profile', False):
                available = False
            if key in ('emulation_thread_cpu_ms_per_frame', 'process_cpu_ms_per_frame'):
                available = available and all(w.get('host_cpu_timing') is True for w in selected)
            if available:
                total = sum(w[key] * (w['frames'] if key in DURATIONS else 1) for w in selected)
                metrics[key] = total / frames
            else:
                metrics[key] = None
        category_mode = report.get('cpu_category_timing', False)
        require(type(category_mode) is bool, name + ': invalid category mode')
        modes.append((category_mode, report.get('instrumented'), report.get('uncapped')))
        category_names = report.get('cpu_category_names', []) if category_mode else []
        if category_mode:
            require(report.get('cpu_category_clock_calibrated') is True,
                    name + ': category clock calibration missing/failed')
            require(category_names and len(set(category_names)) == len(category_names) and
                    all(isinstance(v, str) and v for v in category_names), name + ': invalid category names')
            for w in selected:
                require(w.get('cpu_category_clock_failures') == 0 and
                        w.get('cpu_category_overflows') == 0, name + ': category clock failure/overflow')
                for field in ('cpu_category_ns', 'cpu_category_entries'):
                    require(isinstance(w.get(field), list) and len(w[field]) == len(category_names),
                            name + ': category shape mismatch')
                    for value in w[field]:
                        number(value, name + '/' + field, True)
            for i, category in enumerate(category_names):
                metrics['exclusive_cpu_ms/' + category] = sum(w['cpu_category_ns'][i] for w in selected) / frames / 1e6
                metrics['exclusive_entries/' + category] = sum(w['cpu_category_entries'][i] for w in selected) / frames
        configs.append({k: v for k, v in config.items() if k not in DERIVED})
        runs.append({'name': name, 'guest_cpu_percent': clock, 'refreshes': frames,
                     'window_intervals': interval, 'category_names': category_names,
                     'presentation_metadata': {'report': presentation_metadata(report),
                         'status': presentation_metadata(status),
                         'selected_windows': [dict(presentation_metadata(w), end_frame=w['end_frame'])
                                              for w in selected]},
                     'final_pixel_sha256': config.get('renderer_rgba_sha256'), 'metrics_per_refresh': metrics})
    require(runs and {r['guest_cpu_percent'] for r in runs} == {50, 100}, 'Need both 50% and 100% groups')
    require(all(v == intervals[0] for v in intervals), 'Different selected frame intervals')
    require(all(v == configs[0] for v in configs), 'Other benchmark configuration differs between runs')
    require(all(v == modes[0] for v in modes), 'Diagnostic/pacing modes differ between runs')
    require(all(r['category_names'] == runs[0]['category_names'] for r in runs), 'Category names differ')
    keys = runs[0]['metrics_per_refresh'].keys()
    groups = []
    for clock in (100, 50):
        members = [r for r in runs if r['guest_cpu_percent'] == clock]
        means = {key: (sum(r['metrics_per_refresh'][key] for r in members) / len(members)
                       if all(r['metrics_per_refresh'][key] is not None for r in members) else None)
                 for key in keys}
        groups.append({'guest_cpu_percent': clock, 'run_count': len(members), 'mean_metrics_per_refresh': means})
    hashes = [r['final_pixel_sha256'] for r in runs]
    return {'diagnostic_only': True, 'checkpoint_sha256': manifest['checkpoint_sha256'],
            'title_id': title, 'engine_build_fingerprint': fingerprint,
            'selected_interval': {'after_frame': warmup, 'through_frame': intervals[0][1]},
            'runs': runs, 'groups': groups,
            'final_pixels_identical': len(set(hashes)) == 1 if all(hashes) else None,
            'workload_identical': all(tuple(r['metrics_per_refresh'][k] for k in COUNTS) ==
                                      tuple(runs[0]['metrics_per_refresh'][k] for k in COUNTS) for r in runs),
            'notes': ['Guest-clock diagnostic, not an optimization winner or statistical comparison.',
                      'All values are per emulated display refresh, not unique game frames or wall seconds.',
                      'Raw counters are summed then divided by refreshes; duration means are frame-weighted.',
                      'Group means give each repeated run equal weight; no confidence claims.',
                      'guest_cpu/draw/phase wall scopes overlap and must not be added; exclusive_cpu categories are thread CPU.',
                      'Unavailable or disabled diagnostics are null; zero does not establish absence of uninstrumented calls.',
                      'Underclocking changes graphics commands/writes and guest progress as well as instruction rate.',
                      'PICA command lists/draw triggers and top swaps count requests, not unique frames.',
                      'Stereo top swaps require nonzero distinct physical eye addresses; this does not prove both views rendered.',
                      'presentation_mode describes host output; factor_3d_percent is the guest slider setting.',
                      'Different final pixels/work counts are reported, not rejected; intermediate pixels are not compared.']}


def table(result):
    columns = [(r['name'] + '/' + str(r['guest_cpu_percent']) + '%', r['metrics_per_refresh']) for r in result['runs']]
    columns += [(str(g['guest_cpu_percent']) + '% mean (n=' + str(g['run_count']) + ')', g['mean_metrics_per_refresh']) for g in result['groups']]
    lines = ['Guest-clock diagnostic — not an optimization comparison.',
             'Interval: ' + str(result['selected_interval']),
             '| Per-refresh metric | ' + ' | '.join(c[0] for c in columns) + ' |',
             '| --- | ' + ' | '.join('---:' for _ in columns) + ' |']
    for key in columns[0][1]:
        lines.append('| ' + key + ' | ' + ' | '.join('n/a' if values[key] is None else f'{values[key]:.6f}' for _, values in columns) + ' |')
    lines += ['Final pixel hashes identical: ' + str(result['final_pixels_identical']),
              'Available raw workload counters identical: ' + str(result['workload_identical'])]
    for run in result['runs']:
        observed = {json.dumps({k: v for k, v in w.items() if k != 'end_frame'}, sort_keys=True)
                    for w in run['presentation_metadata']['selected_windows']}
        lines.append(run['name'] + ' observed presentation metadata: ' + '; '.join(sorted(observed)))
    lines += result['notes']
    return '\n'.join(lines)


def self_test():
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        checkpoint = bytearray(112)
        checkpoint[:8] = b'CARAST01'
        struct.pack_into('<Q', checkpoint, 16, 42)
        checkpoint[48:112] = b'a' * 64
        (directory / 'profile-comparison.carastate').write_bytes(checkpoint)
        manifest = {'checkpoint_sha256': hashlib.sha256(checkpoint).hexdigest(), 'runs': []}
        for clock in (100, 50):
            name = 'run' + str(clock)
            manifest['runs'].append({'name': name, 'guest_cpu_percent': clock, 'fingerprint': 'a' * 64,
                                     'renderer_rgba_sha256': name, 'packed': True})
            windows = [{'end_frame': 10, 'frames': 10, 'guest_cpu_percent': clock, 'mean_ms': 2,
                        'pica_command_lists': 3, 'pica_draw_requests': 100,
                        'top_buffer_swaps': 5, 'stereo_top_buffer_swaps': 2,
                        'factor_3d_percent': 0, 'presentation_mode': 'mono_left_top',
                        'gpu_vertex_draws': clock, 'cpu_category_ns': [10000000],
                        'cpu_category_entries': [10], 'cpu_category_clock_failures': 0, 'cpu_category_overflows': 0},
                       {'end_frame': 40, 'frames': 30, 'guest_cpu_percent': clock, 'mean_ms': 6,
                        'pica_command_lists': 9, 'pica_draw_requests': 300,
                        'top_buffer_swaps': 15, 'stereo_top_buffer_swaps': 6,
                        'factor_3d_percent': 0, 'presentation_mode': 'mono_left_top',
                        'gpu_vertex_draws': clock * 3, 'cpu_category_ns': [90000000],
                        'cpu_category_entries': [30], 'cpu_category_clock_failures': 0, 'cpu_category_overflows': 0}]
            report = {'engine_build_fingerprint': 'a' * 64, 'title_id': 42, 'cpu_backend': 'fastinterp',
                      'cpu_category_timing': True, 'cpu_category_clock_calibrated': True,
                      'cpu_category_names': ['cpu'], 'windows': windows}
            status = {'title_id': 42, 'cpu_backend': 'fastinterp', 'jit': False, 'software_renderer': False,
                      'gpu_backend': 'native_gles3', 'result_code': 0, 'frames': 40, 'target_frames': 40,
                      'guest_cpu_percent': clock, 'adaptive_guest_cpu': False}
            audio = {'muted': False, 'queue_running': True, 'last_error': 0, 'nonzero_frames': 1}
            for suffix, value in [('frame-timing', report), ('benchmark-status', status), ('audio-status', audio)]:
                (directory / (name + '-' + suffix + '.json')).write_text(json.dumps(value))
        repeated = dict(manifest['runs'][0], name='repeat100')
        manifest['runs'].append(repeated)
        for suffix in ('frame-timing', 'benchmark-status', 'audio-status'):
            value = read(directory / ('run100-' + suffix + '.json'))
            if suffix == 'frame-timing':
                for w in value['windows']:
                    w['mean_ms'] *= 2
            (directory / ('repeat100-' + suffix + '.json')).write_text(json.dumps(value))
        (directory / 'comparison.json').write_text(json.dumps(manifest))
        result = compare(directory, 0)
        values = result['runs'][0]['metrics_per_refresh']
        assert values['mean_ms'] == 5 and values['gpu_vertex_draws'] == 10
        assert values['pica_command_lists'] == .3 and values['pica_draw_requests'] == 10
        assert values['top_buffer_swaps'] == .5 and values['stereo_top_buffer_swaps'] == .2
        assert result['runs'][0]['presentation_metadata']['selected_windows'][0]['factor_3d_percent'] == 0
        assert values['exclusive_cpu_ms/cpu'] == 2.5 and values['exclusive_entries/cpu'] == 1
        assert result['groups'][0]['run_count'] == 2
        assert result['groups'][0]['mean_metrics_per_refresh']['mean_ms'] == 7.5
        assert result['final_pixels_identical'] is False and result['workload_identical'] is False
        assert values['draw_texture_acquire_calls'] is None
        path = directory / 'run50-frame-timing.json'
        original = read(path)
        for field, bad in [('guest_cpu_percent', 75), ('mean_ms', -1), ('mean_ms', float('nan')),
                           ('cpu_category_clock_failures', 1), ('cpu_category_overflows', 1), ('frames', 29)]:
            changed = json.loads(json.dumps(original))
            changed['windows'][1][field] = bad
            path.write_text(json.dumps(changed))
            try:
                compare(directory, 0)
            except ValueError:
                pass
            else:
                raise AssertionError('accepted bad ' + field)
        path.write_text(json.dumps(original))
        legacy = json.loads(json.dumps(original))
        for w in legacy['windows']:
            for key in ('pica_command_lists', 'pica_draw_requests', 'top_buffer_swaps',
                        'stereo_top_buffer_swaps', 'factor_3d_percent', 'presentation_mode'):
                del w[key]
        path.write_text(json.dumps(legacy))
        assert compare(directory, 0)['runs'][1]['metrics_per_refresh']['top_buffer_swaps'] is None
        path.write_text(json.dumps(original))
        try:
            compare(directory, 5)
        except ValueError:
            pass
        else:
            raise AssertionError('accepted boundary through window')
    print('Guest-clock comparison self-test passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', nargs='?', type=Path)
    parser.add_argument('--warmup', type=int, default=300)
    parser.add_argument('--frames', type=int, help='Selected end boundary; defaults to each completed run')
    parser.add_argument('--json', action='store_true', help='Print structured JSON instead of Markdown table')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if args.directory is None:
        parser.error('directory is required unless --self-test is used')
    try:
        result = compare(args.directory, args.warmup, args.frames)
    except (ValueError, KeyError, TypeError, OSError, UnicodeError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2, allow_nan=False) if args.json else table(result))


if __name__ == '__main__':
    main()
