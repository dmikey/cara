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
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    source = ROOT / 'work' / 'citra-mmj'
    if not (source / '.git' / 'cara-source-stamp').exists():
        raise SystemExit('Run python3 tools/prepare.py first.')
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
    build = ROOT / 'work' / ('build-' + args.platform)
    command = [cmake, '-S', str(source), '-B', str(build), '-G', 'Ninja',
               '-DCMAKE_MAKE_PROGRAM=' + ninja, '-DCMAKE_BUILD_TYPE=Release',
               '-DCITRA_ENABLE_JIT=OFF', '-DCITRA_SOFTWARE_PRESENTATION=ON',
               '-DENABLE_QT=OFF', '-DENABLE_SDL2=OFF', '-DENABLE_WEB_SERVICE=OFF',
               '-DENABLE_CUBEB=OFF', '-DBOOST_ROOT=' + str(source / 'externals/boost'),
               '-DBoost_NO_SYSTEM_PATHS=ON', '-DBoost_NO_BOOST_CMAKE=ON']
    target = 'citra-no-jit-probe'
    if args.platform != 'mac':
        command += ['-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_ARCHITECTURES=arm64',
                    '-DCMAKE_OSX_SYSROOT=' + ('iphoneos' if args.platform == 'ios' else 'iphonesimulator'),
                    '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0',
                    '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY']
        target = 'citra-ios'
    subprocess.run(command, check=True, env=env)
    subprocess.run([cmake, '--build', str(build), '--target', target, '-j', str(args.jobs)], check=True, env=env)


if __name__ == '__main__':
    main()
