#!/usr/bin/env python3
"""Capture a private deterministic iPad replay checkpoint; never commit game data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--device', required=True)
    p.add_argument('--rom', required=True, help='Existing filename in app Documents')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--expected-title-id', type=lambda value:int(value,0), help='Fail if the launched title differs from this known game ID')
    p.add_argument('--frames', type=int, default=2400)
    p.add_argument('--save-frame', type=int, default=2100)
    p.add_argument('--press-a-frames', default='300,600,900,1200')
    p.add_argument('--hold-start-frame', type=int, default=0)
    p.add_argument('--load', action='store_true', help='Continue the current comparison checkpoint')
    p.add_argument('--timeout', type=int, default=360)
    p.add_argument('--settings', default='{}', help='Additional diagnostic environment JSON')
    a = p.parse_args()
    if not 0 < a.save_frame < a.frames <= 18000 or a.frames % 60:
        p.error('save-frame must precede frames, which must be a multiple of 60 <= 18000')
    settings = {'CARA_FRAME_TIMING':'1', 'CARA_HOST_CPU_TIMING':'1',
                'CARA_AUDIO_DIAGNOSTICS':'1', 'CARA_TIMING_WINDOW_FRAMES':'60',
                'CARA_CPU_BACKEND':'fastinterp', 'CARA_ADAPTIVE_GUEST_CPU':'0',
                'CARA_FASTINTERP_PROFILE':'1', 'CARA_TEST_KEEP_AWAKE':'1', 'CARA_BENCHMARK_AUDIO':'1',
                'CARA_BENCHMARK_INPUT_LOCK':'1', 'CARA_BENCHMARK_FRAMES':str(a.frames),
                'CARA_TEST_PRESS_A_FRAMES':a.press_a_frames,
                'CARA_TEST_HOLD_START_FRAME':str(a.hold_start_frame),
                'CARA_PROFILE_SAVE_FRAME':str(a.save_frame)}
    extra=json.loads(a.settings)
    if not isinstance(extra,dict) or any(not isinstance(v,str) for v in extra.values()):
        p.error('settings must be an object of string environment values')
    if set(extra) & (set(settings) | {'CARA_PROFILE_LOAD'}):
        p.error('settings cannot override capture controls; use the dedicated arguments')
    settings.update(extra)
    if a.load:
        settings['CARA_PROFILE_LOAD']='1'
    a.output.mkdir(parents=True, exist_ok=True)
    env=os.environ.copy()
    env.setdefault('DEVELOPER_DIR','/Volumes/Fast/Applications/Xcode.app/Contents/Developer')
    def run(args, quiet=False):
        return subprocess.run(['xcrun','devicectl',*args],env=env,timeout=60,
                              stdout=subprocess.DEVNULL if quiet else None,
                              stderr=subprocess.DEVNULL if quiet else None)
    def copy(name):
        return run(['device','copy','from','--device',a.device,'--source','Documents/cara/'+name,
                    '--destination',str(a.output/name),'--domain-type','appDataContainer',
                    '--domain-identifier','com.dmikey.cara'],True).returncode==0
    run(['device','process','launch','--device',a.device,'--terminate-existing',
         '--environment-variables',json.dumps(settings),'com.dmikey.cara','--','-rom',Path(a.rom).name]).check_returncode()
    started=time.monotonic(); observed=False
    while time.monotonic()-started<a.timeout:
        if copy('frame-timing.json'):
            try:
                timing=json.loads((a.output/'frame-timing.json').read_text())
                if timing.get('rom_filename') != Path(a.rom).name:
                    raise RuntimeError('Different game opened during capture; refusing a mislabelled checkpoint')
                if a.expected_title_id is not None and timing.get('title_id') != a.expected_title_id:
                    raise RuntimeError('Capture title does not match requested game ID')
                frame=timing['windows'][-1]['end_frame']
                if frame<a.frames: observed=True
                if observed and frame>=a.frames and copy('benchmark-status.json'):
                    status=json.loads((a.output/'benchmark-status.json').read_text())
                    if status['frames']==a.frames and status['result_code']==0: break
            except (ValueError,KeyError,IndexError): pass
        if not observed and time.monotonic()-started > 5:
            for diagnostic in ['gpu-vertex-self-test.json', 'surface-coherence-self-test.json','uniform-reuse-self-test.json']:
                if copy(diagnostic):
                    try:
                        check=json.loads((a.output/diagnostic).read_text())
                    except ValueError:
                        continue
                    if check.get('passed') is False:
                        raise RuntimeError(diagnostic+': '+check.get('detail','failed'))
        time.sleep(3)
    else:
        copy('emulator.log')
        raise RuntimeError('Capture did not finish; inspect private emulator.log')
    if not copy('profile-comparison.carastate'):
        raise RuntimeError('Missing comparison checkpoint')
    for name in ['runtime-status.json','audio-status.json','fastinterp-blocks.json','surface-flushes.json','gpu-vertex-self-test.json','surface-coherence-self-test.json','uniform-reuse-self-test.json','emulator.log']:
        copy(name)
    if not copy('profile-checkpoint-status.json'):
        raise RuntimeError('Missing fresh checkpoint save receipt')
    receipt=json.loads((a.output/'profile-checkpoint-status.json').read_text())
    if (receipt.get('saved') is not True or receipt.get('frame') != a.save_frame or
            receipt.get('rom_filename') != Path(a.rom).name or receipt.get('title_id') != timing['title_id'] or
            receipt.get('engine_build_fingerprint') != timing['engine_build_fingerprint']):
        raise RuntimeError('Checkpoint receipt does not match this capture')
    state=a.output/'profile-comparison.carastate'
    manifest={'rom_filename':Path(a.rom).name,'title_id':timing['title_id'],
              'engine_fingerprint':timing['engine_build_fingerprint'],
              'sha256':hashlib.sha256(state.read_bytes()).hexdigest(),
              'settings':settings,'save_frame':a.save_frame,'completed_frame':a.frames}
    (a.output/'checkpoint.json').write_text(json.dumps(manifest,indent=2)+'\n')
    run(['device','capture','screenshot','--device',a.device,
         '--destination',str(a.output/'final.png')]).check_returncode()
    print(json.dumps(manifest),flush=True)


if __name__=='__main__':
    main()
