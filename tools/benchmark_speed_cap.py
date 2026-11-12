#!/usr/bin/env python3
"""Replay an existing compatible private checkpoint to measure iOS speed ceilings.

Runs restart the app. No game images are captured. Results are instrumented wall
clock measurements, with separate uncapped eligibility for saturation claims.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time

HZ = 59.833997376556916
CONFIGS = [('uncapped', None), ('default', None), ('100', 100), ('105', 105), ('200', 200)]


def settings_for(name, percent, frames):
    settings = dict(CARA_FRAME_TIMING='1', CARA_HOST_CPU_TIMING='1',
        CARA_AUDIO_DIAGNOSTICS='1', CARA_BENCHMARK_AUDIO='1',
        CARA_ADAPTIVE_GUEST_CPU='0', CARA_CPU_BACKEND='fastinterp',
        CARA_PROFILE_LOAD='1', CARA_BENCHMARK_FRAMES=str(frames),
        CARA_TIMING_WINDOW_FRAMES='60', CARA_BENCHMARK_INPUT_LOCK='1',
        CARA_TEST_KEEP_AWAKE='1', CARA_TEST_PRESS_A_FRAMES='0',
        CARA_TEST_HOLD_START_FRAME='0', CARA_TEST_CIRCLE_SEGMENTS='',
        CARA_VS_UNIFORM_REUSE='1', CARA_VERTEX_UPLOAD_BANK_MIB='80',
        CARA_GPU_PACKED_INPUTS='1', CARA_VERTEX_UPLOAD_REUSE='1',
        CARA_VERTEX_INDEX_CACHE='1', CARA_GPU_SURFACE_REMAP='1')
    for flag in ('VFP_SIMD', 'RAM_BURST', 'DECODE_BKPT', 'ADD_CMP_IMM', 'VCMP_VMRS'):
        settings['CARA_FASTINTERP_' + flag] = '0'
    for flag in ('CARA_GPU_LIT_VERTEX_REUSE', 'CARA_GPU_VERTEX_REUSE',
                 'CARA_GPU_QUATERNION_SLICE', 'CARA_GPU_PACKED_FETCH_REUSE',
                 'CARA_GPU_RETAIN_INPUT_BINDINGS', 'CARA_GL_STATE_REUSE',
                 'CARA_RETAIN_SMALL_CPU_WRITES', 'CARA_GL_IMPLICIT_FLUSH'):
        settings[flag] = '0'
    if name != 'uncapped':
        settings['CARA_PACED_BENCHMARK'] = '1'
    if percent is not None:
        settings['CARA_MAX_SPEED_PERCENT'] = str(percent)
    return settings


def validate(status, timing, expected, frames, paced, limit):
    if (status.get('result_code') != 0 or status.get('target_frames') != frames or
            not frames <= status.get('frames', -1) <= frames + 4):
        raise ValueError('Failed/incomplete run or unexpected frame overshoot')
    if (expected.get('rom_filename') and status.get('rom_filename') != expected['rom_filename']):
        raise ValueError('ROM filename differs from checkpoint')
    if status.get('title_id') != expected['title_id'] or timing.get('title_id') != expected['title_id']:
        raise ValueError('Checkpoint title mismatch')
    if timing.get('engine_build_fingerprint') != expected['engine_build_fingerprint']:
        raise ValueError('Checkpoint fingerprint mismatch')
    if (status.get('paced') is not paced or timing.get('uncapped') is not (not paced) or
            status.get('guest_cpu_percent') != 100 or status.get('adaptive_guest_cpu') is not False or
            status.get('cpu_backend') != 'fastinterp' or status.get('jit') is not False):
        raise ValueError('Unexpected pace/CPU/no-JIT policy')
    if paced and status.get('maximum_speed_percent') != limit:
        raise ValueError('Effective maximum speed differs from requested/default limit')
    elapsed, wall, paused = (status.get(k) for k in ('elapsed_seconds', 'wall_seconds', 'paused_seconds'))
    if any(not isinstance(x, (int, float)) or not math.isfinite(x) for x in (elapsed, wall, paused)):
        raise ValueError('Invalid benchmark clocks')
    if elapsed <= 0 or wall < elapsed - .01 or paused < 0 or abs(wall-paused-elapsed) > .02:
        raise ValueError('Inconsistent elapsed/wall/pause clocks')
    if paused > .01:
        raise ValueError('Unexpected pause during controlled benchmark')
    fps = status['frames'] / elapsed
    if not math.isfinite(status.get('fps', float('nan'))) or abs(status['fps']-fps) > .02:
        raise ValueError('Reported FPS does not count all actual rendered frames')
    if paced and fps > HZ * limit / 100 * 1.005:
        raise ValueError('Speed ceiling exceeded')
    return fps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True)
    parser.add_argument('--rom', required=True, help='ROM filename already in app Documents')
    parser.add_argument('--output', type=Path, required=True, help='New private artifact directory')
    parser.add_argument('--bundle', default='com.dmikey.cara')
    parser.add_argument('--frames', type=int, default=600)
    parser.add_argument('--warmup', type=int, default=120)
    parser.add_argument('--timeout', type=int, default=180)
    args = parser.parse_args()
    if not 60 <= args.warmup < args.frames <= 18000 or args.warmup % 60 or args.frames % 60 or args.timeout < 20:
        parser.error('frames/warmup must be multiples of60 with60<=warmup<frames<=18000; timeout>=20')
    if Path(args.rom).name != args.rom:
        parser.error('--rom must be a filename, not a path')
    args.output.mkdir(parents=True, exist_ok=False)
    env = {k: v for k, v in os.environ.items() if not k.startswith('DEVICECTL_CHILD_')}
    env.setdefault('DEVELOPER_DIR', '/Volumes/Fast/Applications/Xcode.app/Contents/Developer')
    def run(command, quiet=True):
        return subprocess.run(['xcrun', 'devicectl', *command], env=env, timeout=45,
            stdout=subprocess.DEVNULL if quiet else None, stderr=subprocess.DEVNULL if quiet else None)
    def copy(name, destination):
        return run(['device', 'copy', 'from', '--device', args.device,
            '--source', 'Documents/cara/' + name, '--destination', str(destination),
            '--domain-type', 'appDataContainer', '--domain-identifier', args.bundle]).returncode == 0
    def read(name, destination):
        if copy(name, destination):
            try:
                return json.loads(destination.read_text())
            except (ValueError, OSError):
                pass
        return None
    checkpoint = args.output / 'checkpoint.tmp'
    if not copy('profile-comparison.carastate', checkpoint):
        raise RuntimeError('Existing compatible profile checkpoint required')
    checkpoint_sha = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
    checkpoint.unlink()  # Preserve metadata/hash only, not a second copy of game data.
    expected = read('profile-checkpoint-status.json', args.output / 'checkpoint-metadata.json')
    if expected and expected.get('rom_filename') != args.rom:
        raise RuntimeError('Requested ROM differs from captured checkpoint')
    if not expected or not expected.get('saved') or not expected.get('engine_build_fingerprint') or not expected.get('title_id'):
        raise RuntimeError('Checkpoint identity/fingerprint metadata required')
    results = dict(device=args.device, bundle=args.bundle, checkpoint_sha256=checkpoint_sha, checkpoint=expected, rom=args.rom,
        frames=args.frames, warmup=args.warmup,
        methodology='Instrumented actual frames / benchmark elapsed wall time; windows are diagnostic only', runs=[])
    for name, percent in CONFIGS:
        directory = args.output / name
        directory.mkdir()
        old_timing = directory / 'before-timing.json'
        old_status = directory / 'before-status.json'
        copy('frame-timing.json', old_timing)
        copy('benchmark-status.json', old_status)
        old_timing_bytes = old_timing.read_bytes() if old_timing.exists() else b''
        old_status_bytes = old_status.read_bytes() if old_status.exists() else b''
        settings = settings_for(name, percent, args.frames)
        (directory / 'environment.json').write_text(json.dumps(settings, indent=2)+'\n')
        run(['device', 'process', 'launch', '--device', args.device, '--terminate-existing',
            '--environment-variables', json.dumps(settings), args.bundle, '--', '-rom', args.rom], False).check_returncode()
        started = time.monotonic()
        fresh_incomplete = False
        status = timing = None
        while time.monotonic()-started < args.timeout:
            timing = read('frame-timing.json', directory / 'frame-timing.json')
            if timing and timing.get('title_id') == expected['title_id'] and timing.get('engine_build_fingerprint') == expected['engine_build_fingerprint']:
                windows = timing.get('windows', [])
                end = windows[-1].get('end_frame', 0) if windows else 0
                changed = (directory / 'frame-timing.json').read_bytes() != old_timing_bytes
                right_mode = timing.get('uncapped') is (name == 'uncapped')
                if changed and right_mode and 0 < end < args.frames:
                    fresh_incomplete = True
                if fresh_incomplete and right_mode and end >= args.frames:
                    status = read('benchmark-status.json', directory / 'benchmark-status.json')
                    if status and (directory / 'benchmark-status.json').read_bytes() != old_status_bytes:
                        break
            time.sleep(.25)
        else:
            copy('emulator.log', directory / 'failure.log')
            raise RuntimeError('No fresh incomplete -> complete report: ' + name)
        limit = percent if percent is not None else 105
        fps = validate(status, timing, expected, args.frames, name != 'uncapped', limit)
        windows = [w for w in timing['windows'] if args.warmup < w['end_frame'] <= status['frames']]
        if len(windows) != (args.frames-args.warmup)//60 or any(w.get('guest_cpu_percent') != 100 for w in windows):
            raise RuntimeError('Missing diagnostic windows or guest clock changed')
        audio = read('audio-status.json', directory / 'audio-status.json')
        if not audio or audio.get('muted') is not False or audio.get('manually_paused') is not False or audio.get('last_error') != 0:
            raise RuntimeError('Audio-on evidence missing, muted, paused, or failed')
        copy('runtime-status.json', directory / 'runtime-status.json')
        # Ensure no external capture/replacement changed the checkpoint mid-suite.
        if not copy('profile-comparison.carastate', checkpoint):
            raise RuntimeError('Checkpoint disappeared')
        current_sha = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
        checkpoint.unlink()
        if current_sha != checkpoint_sha:
            raise RuntimeError('Checkpoint changed during suite')
        control_fps = results['runs'][0]['fps'] if results['runs'] else fps
        eligible = name != 'uncapped' and control_fps >= HZ*limit/100*1.10
        entry = dict(name=name, fps=fps, frames=status['frames'], elapsed_seconds=status['elapsed_seconds'],
            wall_seconds=status['wall_seconds'], paused_seconds=status['paused_seconds'],
            fresh_incomplete_observed=fresh_incomplete, maximum_speed_percent=limit if name!='uncapped' else None,
            ceiling_pass=True if name!='uncapped' else None, saturation_eligible=eligible,
            pace_efficiency=fps/(HZ*limit/100) if name!='uncapped' else None,
            saturation_pass=(fps >= HZ*limit/100*.97) if eligible else None,
            diagnostic_windows=len(windows))
        results['runs'].append(entry)
        (args.output / 'results.json').write_text(json.dumps(results, indent=2)+'\n')
        print(name, json.dumps(entry), flush=True)
    print('Ceilings passed. Saturation requires saturation_eligible=true; slower scenes prove only the ceiling.')


if __name__ == '__main__':
    main()
