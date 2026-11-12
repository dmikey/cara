#!/usr/bin/env python3
"""Replay a private Cara profiling checkpoint on iOS; never add game data to Git."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bundle', default='com.dmikey.cara')
    parser.add_argument('--rom', help='Pin a ROM filename already present in the app Documents directory')
    parser.add_argument('--frames', type=int, default=1200)
    parser.add_argument('--warmup', type=int, default=300)
    parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('--config', action='append', required=True,
                        help='JSON object: name, packed (boolean), bank_mib (16..128), optional reuse and index_cache (booleans)')
    parser.add_argument('--resume-best', action='store_true')
    parser.add_argument('--screenshots', action='store_true')
    parser.add_argument('--press-a-frames', default='0',
                        help='Comma-separated emulated frames for six-frame A presses; 0 disables')
    parser.add_argument('--host-cpu-timing', action='store_true')
    parser.add_argument('--circle-segments', default='',
                        help='Comma-separated begin:end:x:y stick movements; neutral between segments')
    args = parser.parse_args()
    if args.frames <= args.warmup or args.frames % 60 or args.warmup % 60:
        parser.error('frames and warmup must be multiples of 60, with frames > warmup')
    try:
        press_frames = [int(value) for value in args.press_a_frames.split(',')]
    except ValueError:
        parser.error('press-a-frames must contain integers')
    if any(value < 0 or value >= args.frames for value in press_frames):
        parser.error('A press frames must be within the run')
    import math
    circle_segments = []
    try:
        for item in filter(None, args.circle_segments.split(',')):
            begin, end, x, y = item.split(':')
            begin, end, x, y = int(begin), int(end), float(x), float(y)
            if not (0 <= begin < end <= args.frames and math.isfinite(x) and math.isfinite(y)
                    and abs(x) <= 1 and abs(y) <= 1):
                raise ValueError('invalid circle segment')
            if circle_segments and begin < circle_segments[-1][1]:
                raise ValueError('segments must be ordered and nonoverlapping')
            circle_segments.append((begin, end, x, y))
    except ValueError:
        parser.error('circle-segments require ordered begin:end:x:y values within the run and [-1,1]')
    configs = [json.loads(value) for value in args.config]
    for config in configs:
        if (not isinstance(config.get('index_cache', False), bool) or not isinstance(config.get('reuse', False), bool) or not isinstance(config['packed'], bool) or not isinstance(config['bank_mib'], int)
                or not 16 <= config['bank_mib'] <= 128
                or not config['name'].replace('_', '').replace('-', '').isalnum()):
            parser.error('invalid configuration')
    if len({config['name'] for config in configs}) != len(configs):
        parser.error('configuration names must be unique')
    args.output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.setdefault('DEVELOPER_DIR', '/Volumes/Fast/Applications/Xcode.app/Contents/Developer')

    def run(command, quiet=False):
        return subprocess.run(['xcrun', 'devicectl', *command], env=env, timeout=60,
                              stdout=subprocess.DEVNULL if quiet else None,
                              stderr=subprocess.DEVNULL if quiet else None)

    def copy(name, target):
        return run(['device', 'copy', 'from', '--device', args.device,
                    '--source', 'Documents/cara/' + name, '--destination', str(target),
                    '--domain-type', 'appDataContainer', '--domain-identifier', args.bundle], True).returncode == 0

    checkpoint = args.output / 'profile-comparison.carastate'
    if not copy(checkpoint.name, checkpoint):
        raise RuntimeError('Capture a comparison checkpoint in Cara before running this tool')
    results = {'checkpoint_sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(), 'press_a_frames': press_frames, 'circle_segments': circle_segments, 'touch_input_locked': True, 'runs': []}

    def launch(config, benchmark=True):
        settings = {'CARA_FRAME_TIMING': '1', 'CARA_AUDIO_DIAGNOSTICS': '1',
                    'CARA_OPEN_RECENT': '1', 'CARA_TIMING_WINDOW_FRAMES': '60',
                    'CARA_CPU_BACKEND': 'fastinterp', 'CARA_PROFILE_LOAD': '1',
                    'CARA_TEST_PRESS_A_FRAMES': args.press_a_frames if benchmark else '0',
                    'CARA_BENCHMARK_INPUT_LOCK': '1' if benchmark else '0',
                    'CARA_TEST_CIRCLE_SEGMENTS': args.circle_segments if benchmark else '',
                    'CARA_TEST_HOLD_START_FRAME': '0',
                    'CARA_PROFILE_SAVE_FRAME': '0',
                    'CARA_GPU_PACKED_INPUTS': '1' if config['packed'] else '0',
                    'CARA_VERTEX_UPLOAD_REUSE': '1' if config.get('reuse', False) else '0',
                    'CARA_VERTEX_INDEX_CACHE': '1' if config.get('index_cache', False) else '0',
                    'CARA_VERTEX_UPLOAD_BANK_MIB': str(config['bank_mib']),
                    'CARA_ADAPTIVE_GUEST_CPU': '0' if benchmark else '1'}
        if args.host_cpu_timing:
            settings['CARA_HOST_CPU_TIMING'] = '1'
        if benchmark:
            settings['CARA_BENCHMARK_FRAMES'] = str(args.frames)
            settings['CARA_BENCHMARK_AUDIO'] = '1'
        arguments = []
        if args.rom:
            settings.pop('CARA_OPEN_RECENT')
            arguments = ['--', '-rom', Path(args.rom).name]
        run(['device', 'process', 'launch', '--device', args.device, '--terminate-existing',
             '--environment-variables', json.dumps(settings), args.bundle, *arguments]).check_returncode()

    for config in configs:
        launch(config)
        print('Started', config['name'], flush=True)
        time.sleep(3)
        started = time.monotonic()
        target = args.output / (config['name'] + '-frame-timing.json')
        # Observe a new, incomplete run before accepting completion, so an old
        # report cannot be mistaken for the just-launched process.
        observed_start = False
        while time.monotonic() - started < args.timeout:
            if copy('frame-timing.json', target):
                try:
                    report = json.loads(target.read_text())
                    window = report['windows'][-1]
                    right_mode = ((window['gpu_packed_input_draws'] > 0) == config['packed']
                                  and window['vertex_upload_bank_budget_bytes'] == config['bank_mib'] * 1048576)
                    if right_mode and window['end_frame'] < args.frames:
                        observed_start = True
                    if observed_start and right_mode and window['end_frame'] >= args.frames:
                        break
                except (ValueError, KeyError, IndexError):
                    pass  # Report may be in the middle of a write.
            time.sleep(5)
        else:
            copy('emulator.log', args.output / (config['name'] + '-failure.log'))
            raise RuntimeError('Run did not complete: ' + config['name'])
        windows = [w for w in report['windows'] if args.warmup < w['end_frame'] <= args.frames]
        if len(windows) != (args.frames - args.warmup) // 60:
            raise RuntimeError('Incomplete measurement window')
        if any(w['guest_cpu_percent'] != 100 or w['packed_validation_failures'] != 0
               or w['packed_verified_draws'] != 0 for w in windows):
            raise RuntimeError('Unexpected clock, validation failure or active shadow decoder')
        fields = ['mean_ms', 'vertex_prepare_ms_per_frame', 'vertex_upload_ms_per_frame',
                  'draw_submit_ms_per_frame', 'vertex_upload_bytes', 'gpu_vertices',
                  'gpu_vertex_draws', 'gpu_packed_input_draws', 'vertex_upload_fallbacks',
                  'vertex_upload_reused_bytes', 'vertex_upload_reused_textures',
                  'vertex_reuse_check_ms_per_frame', 'vertex_index_cache_hits',
                  'vertex_index_cache_misses', 'vertex_index_cache_bytes']
        if args.host_cpu_timing:
            fields += ['emulation_thread_cpu_ms_per_frame', 'process_cpu_ms_per_frame']
        means = {key: sum(w[key] for w in windows) / len(windows) for key in fields}
        result = dict(config, fingerprint=report['engine_build_fingerprint'], means=means)
        results['runs'].append(result)
        for name in ['runtime-status.json', 'audio-status.json', 'benchmark-status.json']:
            if not copy(name, args.output / (config['name'] + '-' + name)):
                raise RuntimeError('Unable to read ' + name)
        (args.output / 'comparison.json').write_text(json.dumps(results, indent=2) + '\n')
        if args.screenshots:
            run(['device', 'capture', 'screenshot', '--device', args.device,
                 '--destination', str(args.output / (config['name'] + '.png'))]).check_returncode()
        print(config['name'], round(means['mean_ms'], 3), 'ms per refresh', flush=True)
    fingerprints = {r['fingerprint'] for r in results['runs']}
    if len(fingerprints) != 1:
        raise RuntimeError('Runs used different engine builds')
    results['matched_workload'] = len({(r['means']['gpu_vertices'], r['means']['gpu_vertex_draws'])
                                      for r in results['runs']}) == 1
    (args.output / 'comparison.json').write_text(json.dumps(results, indent=2) + '\n')
    if args.resume_best:
        if not results['matched_workload']:
            raise RuntimeError('Workloads differ; refusing to choose a winner automatically')
        best = min(results['runs'], key=lambda r: r['means']['mean_ms'])
        launch(best, benchmark=False)
        print('Resumed adaptive play:', best['name'], flush=True)


if __name__ == '__main__':
    main()
