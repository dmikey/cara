#!/usr/bin/env python3
"""Build the opt-in native Metal backend with pinned static shader compilers."""
# SPDX-License-Identifier: GPL-2.0-or-later
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'emulator'
COMPILERS = (
    ('glslang', 'KhronosGroup/glslang', 'e1b562a8bed273a02f30b59b66a5d499793cede5',
     'glslang/Public/ShaderLang.h', 'LICENSE.txt'),
    ('spirv-cross', 'KhronosGroup/SPIRV-Cross', '1a6169566c73d3da552748fc372fe2bbb856e46e',
     'spirv_msl.hpp', 'LICENSE'),
)


def run(args, **options):
    subprocess.run(args, check=True, **options)


def read(args, **options):
    return subprocess.check_output(args, text=True, **options).strip()


def pinned_source(cache, dependency, allow_fetch):
    name, upstream, revision, marker, _ = dependency
    source = cache / name
    if not source.exists():
        if not allow_fetch:
            raise RuntimeError(f'Missing pinned shader compiler checkout: {source}')
        cache.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=f'{name}-', dir=cache) as scratch:
            checkout = Path(scratch) / name
            run(['git', 'init', '--quiet', str(checkout)])
            run(['git', '-C', str(checkout), 'fetch', '--no-tags', '--depth=1',
                 f'https://github.com/{upstream}.git', revision])
            run(['git', '-C', str(checkout), 'checkout', '--detach', '--quiet', 'FETCH_HEAD'])
            if read(['git', '-C', str(checkout), 'rev-parse', 'HEAD']) != revision:
                raise RuntimeError(f'Wrong fetched revision for {name}')
            checkout.rename(source)
    if not (source / '.git').exists() or not (source / marker).is_file():
        raise RuntimeError(f'Shader compiler source is incomplete: {source}')
    git = ['git', '-C', str(source)]
    if read([*git, 'rev-parse', 'HEAD']) != revision or read([*git, 'status', '--porcelain']):
        raise RuntimeError(f'{name} requires a clean checkout of {revision}: {source}')
    return source


def configure_args(args, build, deps, cmake, ninja):
    mac = args.platform == 'mac'
    switches = {
        'CARA_ENABLE_JIT': False,
        'CARA_MAC_CPU_JIT': args.cpu_jit,
        'CARA_MAC_SDL_DEV_HOST': mac,
        'CARA_MAC_ATLAS_PIPELINE': mac,
        'CARA_IOS_HARDWARE_RENDERER': not mac,
        'CARA_PRIVATE_MAC_METAL_HOST': mac,
        'CARA_PRIVATE_IOS_METAL_LCD': not mac,
        'CARA_PRIVATE_METAL_DRAWS': True,
        'CARA_PRIVATE_METAL_TRANSFERS': True,
        'CARA_SOFTWARE_PRESENTATION': False,
        'CARA_INTERPRETER_GUEST_DEBUG': False,
        'ENABLE_SDL2': mac,
        'ENABLE_QT': False,
        'ENABLE_WEB_SERVICE': False,
        'ENABLE_CUBEB': False,
        'ENABLE_FFMPEG_AUDIO_DECODER': False,
        'ENABLE_FFMPEG_VIDEO_DUMPER': False,
        'Boost_NO_SYSTEM_PATHS': True,
        'Boost_NO_BOOST_CMAKE': True,
    }
    command = [cmake, '-S', str(SOURCE), '-B', str(build), '-G', 'Ninja',
               f'-DCMAKE_MAKE_PROGRAM={ninja}', '-DCMAKE_BUILD_TYPE=Release',
               '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCARA_CPU_TARGET=generic',
               '-DCARA_PRIVATE_GATE_METAL_CATALOG:PATH=',
               f'-DCARA_METAL_GLSLANG_SOURCE:PATH={deps[0]}',
               f'-DCARA_METAL_SPIRV_CROSS_SOURCE:PATH={deps[1]}',
               f'-DBOOST_ROOT={SOURCE / "externals/boost"}']
    command.extend(f'-D{name}={"ON" if value else "OFF"}' for name, value in switches.items())
    if mac:
        prefix = args.prefix_path or Path(read(['brew', '--prefix']))
        command.extend(['-DCMAKE_OSX_SYSROOT=macosx', f'-DCMAKE_PREFIX_PATH={prefix.resolve()}'])
    else:
        command.extend(['-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_SYSROOT=iphoneos',
                        '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0',
                        '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY'])
    return command


def bundle_and_audit(build, target, mac, cpu_jit, deps):
    app = build / 'bin' / (target + '.app')
    resources = app / 'Contents/Resources' if mac else app
    binary = app / 'Contents/MacOS' / target if mac else app / target
    resource_hashes = {}
    for name in ('cara-pica.metallib', 'cara-scaled-crop-rgb8.metallib'):
        resource_hashes[name] = hashlib.sha256((resources / name).read_bytes()).hexdigest()
    if (resources / 'cara-gate.metallib').exists():
        raise RuntimeError('Unexpected private title catalog in the open Metal bundle')

    resources.mkdir(parents=True, exist_ok=True)
    for (name, _, _, _, notice), checkout in zip(COMPILERS, deps):
        licenses = resources / 'Licenses' / name
        licenses.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(checkout / notice, licenses / notice)
        shutil.copytree(checkout / 'LICENSES', licenses / 'LICENSES', dirs_exist_ok=True)

    # Keep this developer artifact runnable on the build Mac with the same
    # isolated CPU JIT entitlement used by the existing macOS backend.
    if mac:
        sign = ['codesign', '--force', '--sign', '-']
        if cpu_jit:
            entitlements = build / 'native-metal-jit-entitlements.plist'
            entitlements.write_bytes(plistlib.dumps({'com.apple.security.cs.allow-jit': True}))
            sign.extend(['--entitlements', str(entitlements)])
        run([*sign, str(app)])
        run(['codesign', '--verify', '--strict', str(app)])

    if read(['lipo', '-archs', str(binary)]) != 'arm64':
        raise RuntimeError('The native Metal bundle must contain only arm64')
    graph = (build / 'build.ninja').read_text()
    for required in ('pica_metal_native_renderer.mm.o', 'pica_metal_native_draw_lane.mm.o',
                     'pica_metal_runtime_shader.cpp.o', 'cara-scaled-crop-rgb8.metallib'):
        if required not in graph:
            raise RuntimeError(f'Missing Metal build input: {required}')
    if 'shader/shader_jit_x64.cpp.o' in graph:
        raise RuntimeError('Unexpected shader JIT in the native Metal build')
    symbols = read(['nm', str(binary)])
    if 'JitX64Engine' in symbols or ('ARM_Dynarmic' in symbols) != cpu_jit:
        raise RuntimeError('Linked JIT backend differs from the requested policy')

    libs = read(['otool', '-L', str(binary)]).splitlines()[1:]
    linkage = [line.strip().split(' (', 1)[0] for line in libs]
    if any('glslang' in lib.lower() or 'spirv-cross' in lib.lower() for lib in linkage):
        raise RuntimeError('Shader compiler dependencies must link statically')
    if not mac:
        if any(not lib.startswith(('/System/Library/', '/usr/lib/')) for lib in linkage):
            raise RuntimeError('Unexpected third-party dylib in the iOS build')
        if re.search(r'\b_(mmap|mprotect|vm_protect)$', symbols, flags=re.MULTILINE):
            raise RuntimeError('Unexpected executable-memory symbols in the iOS build')
    report = {
        'source_revision': read(['git', 'rev-parse', 'HEAD'], cwd=ROOT),
        'tracked_source_modified': bool(read(
            ['git', 'status', '--porcelain', '--untracked-files=no', '--', 'emulator'], cwd=ROOT)),
        'renderer': 'native_metal',
        'architecture': 'arm64',
        'platform': 'mac' if mac else 'ios',
        'cpu_jit_enabled': cpu_jit,
        'shader_jit_enabled': False,
        'runtime_shader_compilers': {name: revision for name, _, revision, _, _ in COMPILERS},
        'runtime_metal_libraries': resource_hashes,
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'dynamic_dependencies': linkage,
    }
    driver = build / 'native-metal-audit.json'
    driver.write_text(json.dumps(report, indent=2) + '\n')
    print(app)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('platform', choices=['mac', 'ios'])
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--shader-sources', type=Path,
                        help='Reuse offline, clean pinned checkouts in glslang/ and spirv-cross/')
    parser.add_argument('--prefix-path', type=Path, help='macOS SDL2 and libpng dependency prefix')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--cpu-jit', action='store_true', help='Include the optional macOS CPU JIT')
    parser.add_argument('--configure-only', action='store_true')
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('Native Metal builds require an Apple Silicon Mac with Xcode selected')
    if args.platform == 'ios' and (args.cpu_jit or args.prefix_path):
        parser.error('--cpu-jit and --prefix-path apply only to the macOS build')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    cmake, ninja = shutil.which('cmake'), shutil.which('ninja')
    if not cmake or not ninja:
        parser.error('Install CMake 3.31.10 and Ninja 1.13.2')
    if args.platform == 'mac' and not args.prefix_path and not shutil.which('brew'):
        parser.error('Install SDL2 and libpng with Homebrew or provide --prefix-path')
    sdk = 'macosx' if args.platform == 'mac' else 'iphoneos'
    run(['xcrun', '--sdk', sdk, '--find', 'metal'], stdout=subprocess.DEVNULL)
    run(['xcrun', '--sdk', sdk, '--find', 'metallib'], stdout=subprocess.DEVNULL)
    build = (args.build_dir or ROOT / 'work' / ('build-native-metal-' + args.platform)).resolve()
    cache = args.shader_sources.resolve() if args.shader_sources else build / 'shader-sources'
    deps = tuple(pinned_source(cache, dependency, not args.shader_sources) for dependency in COMPILERS)
    build.mkdir(parents=True, exist_ok=True)
    command = configure_args(args, build, deps, cmake, ninja)
    (build / 'native-metal-configure.json').write_text(json.dumps(command, indent=2) + '\n')
    run(command)
    if args.configure_only:
        return
    target = 'cara-metal-rom' if args.platform == 'mac' else 'cara-ios'
    run([cmake, '--build', str(build), '--target', target, '--parallel', str(args.jobs)])
    bundle_and_audit(build, target, args.platform == 'mac', args.cpu_jit, deps)


if __name__ == '__main__':
    main()
