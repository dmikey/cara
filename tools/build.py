#!/usr/bin/env python3
"""Configure/build a no-JIT macOS probe or unsigned ARM64 iOS app."""
import argparse
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('platform', choices=['mac', 'ios', 'simulator'])
    parser.add_argument('--renderer', choices=['software', 'gles3'], default='software')
    parser.add_argument('--jobs', type=int, default=3)
    parser.add_argument('--guest-debugger', action='store_true', help='Include per-instruction guest GDB checks')
    parser.add_argument('--cpu', choices=['generic', 'apple-m4'], default='generic')
    parser.add_argument('--lto', action='store_true')
    pgo = parser.add_mutually_exclusive_group()
    pgo.add_argument('--pgo-generate', action='store_true')
    pgo.add_argument('--pgo-use', type=Path)
    parser.add_argument('--build-dir', type=Path)
    args = parser.parse_args()
    if args.cpu == 'apple-m4' and args.platform != 'ios':
        parser.error('apple-m4 requires a physical-device iOS build')
    if args.pgo_use and not args.pgo_use.is_file():
        parser.error('PGO profile file does not exist')
    source = ROOT / 'emulator'
    if not (source / 'VENDORED_SOURCE.json').exists():
        raise SystemExit('Bundled emulator source is missing.')
    env = os.environ.copy()
    if not env.get('DEVELOPER_DIR'):
        for app in ['/Applications/Xcode.app', '/Volumes/Fast/Applications/Xcode.app']:
            if Path(app).is_dir():
                env['DEVELOPER_DIR'] = app + '/Contents/Developer'
                break
    local = ROOT / 'work' / 'tools' / 'bin'
    if local.is_dir():
        env['PATH'] = str(local) + os.pathsep + env['PATH']
    cmake = shutil.which('cmake', path=env['PATH'])
    ninja = shutil.which('ninja', path=env['PATH'])
    if not cmake or not ninja:
        raise SystemExit('Install CMake 3.31.10 and Ninja; see README.')
    if args.renderer == 'gles3' and args.platform == 'mac':
        raise SystemExit('The native GLES3 backend targets iOS only.')
    suffix = '-gles3' if args.renderer == 'gles3' else ''
    build = ROOT / 'work' / ('build-' + args.platform + suffix)
    if args.build_dir:
        build = args.build_dir.resolve()
    command = [cmake, '-S', str(source), '-B', str(build), '-G', 'Ninja',
               '-DCMAKE_MAKE_PROGRAM=' + ninja, '-DCMAKE_BUILD_TYPE=Release',
               '-DCARA_CPU_TARGET=' + args.cpu,
               '-DCARA_ENABLE_LTO=' + ('ON' if args.lto else 'OFF'),
               '-DCARA_PGO_GENERATE=' + ('ON' if args.pgo_generate else 'OFF'),
               '-DCARA_PGO_USE=' + (str(args.pgo_use.resolve()) if args.pgo_use else ''),
               '-DCARA_INTERPRETER_GUEST_DEBUG=' + ('ON' if args.guest_debugger else 'OFF'),
               '-DCARA_ENABLE_JIT=OFF', '-DCARA_SOFTWARE_PRESENTATION=ON',
               '-DENABLE_QT=OFF', '-DENABLE_SDL2=OFF', '-DENABLE_WEB_SERVICE=OFF',
               '-DENABLE_CUBEB=OFF', '-DBOOST_ROOT=' + str(source / 'externals/boost'),
               '-DBoost_NO_SYSTEM_PATHS=ON', '-DBoost_NO_BOOST_CMAKE=ON']
    targets = ['cara-render-bookkeeping-self-test', 'cara-cpu-category-self-test', 'cara-vertex-upload-bank-self-test', 'cara-frame-pacer-self-test', 'cara-clock-self-test', 'cara-fastinterp-self-test', 'cara-audio-buffer-self-test', 'cara-no-jit-probe', 'cara-raster-self-test', 'cara-core-cache-self-test', 'cara-interval-cache-self-test', 'cara-input-self-test']
    if args.platform == 'mac':
        targets.extend(['cara-skin-self-test', 'cara-library-self-test', 'cara-play-profile-self-test', 'cara-input-hub-self-test', 'cara-state-catalog-self-test', 'cara-configuration-self-test', 'cara-save-sync-self-test', 'cara-save-state-self-test', 'cara-extdata-state-self-test'])
    if args.platform != 'mac':
        command += ['-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_ARCHITECTURES=arm64',
                    '-DCMAKE_OSX_SYSROOT=' + ('iphoneos' if args.platform == 'ios' else 'iphonesimulator'),
                    '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0',
                    '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
                    '-DCARA_IOS_HARDWARE_RENDERER=' + ('ON' if args.renderer == 'gles3' else 'OFF')]
        targets = ['cara-ios']
    subprocess.run(command, check=True, env=env)
    subprocess.run([cmake, '--build', str(build), '--target', *targets, '-j', str(args.jobs)], check=True, env=env)


if __name__ == '__main__':
    main()
