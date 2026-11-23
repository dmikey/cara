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
import shlex
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
        'CARA_ENABLE_LTO': args.lto,
        'CARA_FASTINTERP_PGO_GENERATE': getattr(args, 'fastinterp_pgo_generate', False),
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
    profile = getattr(args, 'fastinterp_pgo_use', None)
    command.append('-DCARA_FASTINTERP_PGO_USE:FILEPATH=' + (str(profile.resolve()) if profile else ''))
    command.append('-DCARA_FASTINTERP_PGO_KIND:STRING=' + getattr(args, 'fastinterp_pgo_kind', 'frontend'))
    if mac:
        prefix = args.prefix_path or Path(read(['brew', '--prefix']))
        command.extend(['-DCMAKE_OSX_SYSROOT=macosx', f'-DCMAKE_PREFIX_PATH={prefix.resolve()}'])
    else:
        command.extend(['-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_SYSROOT=iphoneos',
                        '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0',
                        '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY'])
    return command



def ninja_build_fields(graph, output):
    match = re.search(r'^build ' + re.escape(output) + r': [^\n]*\n((?:  [^\n]*\n)*)',
                      graph, flags=re.MULTILINE)
    if not match:
        raise RuntimeError(f'Missing compiler policy build recipe: {output}')
    return dict(re.findall(r'^  ([A-Z_]+) = (.*)$', match[1], flags=re.MULTILINE))


def lto_mode(flags):
    """Report the effective driver option, preserving explicit -fno-lto order."""
    mode = None
    for option in shlex.split(flags):
        if option == '-fno-lto':
            mode = None
        elif option == '-flto':
            mode = 'full'
        elif option.startswith('-flto='):
            mode = option.partition('=')[2]
    return mode


def read_cmake_cache(build):
    return dict(re.findall(r'^([A-Za-z_][A-Za-z_0-9]*):[^=\r\n]+=(.*)$',
                           (build / 'CMakeCache.txt').read_text(), flags=re.MULTILINE))


def objcxx_lto_override(build, enabled):
    """Own only our appended ThinLTO flag, retaining existing Release flags.

    The sidecar lets a later default/OFF configuration restore the exact prior
    flags. Unrecognized edits are rejected rather than overwritten. Additional
    trailing user flags are preserved when removing our known prefix.
    """
    record_path = build / 'native-metal-objcxx-lto.json'
    if not (build / 'CMakeCache.txt').exists():
        return None
    current = read_cmake_cache(build).get('CMAKE_OBJCXX_FLAGS_RELEASE')
    if current is None:
        return None
    base = current
    if record_path.exists():
        record = json.loads(record_path.read_text())
        applied, original = record['applied_release_flags'], record['base_release_flags']
        if current == applied:
            base = original
        elif current.startswith(applied + ' '):
            base = original + current[len(applied):]
        else:
            raise RuntimeError('ObjC++ Release flags changed outside the managed LTO override; refusing to overwrite custom flags')
    elif not enabled:
        return None
    desired = base + ' -flto=thin' if enabled else base
    record = {'base_release_flags': base, 'applied_release_flags': desired} if enabled else None
    return desired, record


def commit_objcxx_lto_override(build, override):
    if override is None:
        return
    desired, record = override
    if read_cmake_cache(build).get('CMAKE_OBJCXX_FLAGS_RELEASE') != desired:
        raise RuntimeError('CMake did not retain the requested ObjC++ Release flags')
    path = build / 'native-metal-objcxx-lto.json'
    if record is None:
        path.unlink(missing_ok=True)
    else:
        path.write_text(json.dumps(record, indent=2) + '\n')


def profile_instrumentation_kind(profile, compiler):
    if not compiler:
        raise RuntimeError('Missing configured compiler for exact-toolchain profile kind audit')
    profdata = Path(compiler).with_name('llvm-profdata')
    if not profdata.is_file():
        raise RuntimeError('Matching compiler toolchain llvm-profdata is unavailable')
    output = read([str(profdata), 'show', '--profile-version', str(profile)])
    match = re.search(r'^Instrumentation level: (Front-end|IR)\b', output, flags=re.MULTILINE)
    if not match:
        raise RuntimeError('Cannot establish the supplied LLVM profile instrumentation kind')
    return 'frontend' if match[1] == 'Front-end' else 'ir'


def audit_fastinterp_pgo(build, units, link, requested_generate=False, requested_use=None, requested_kind='frontend'):
    cache = read_cmake_cache(build)
    generate = cache.get('CARA_FASTINTERP_PGO_GENERATE', 'OFF').upper() in ('ON', 'TRUE', 'YES', '1')
    use = cache.get('CARA_FASTINTERP_PGO_USE', '')
    expected_use = str(Path(requested_use).resolve()) if requested_use else ''
    kind = cache.get('CARA_FASTINTERP_PGO_KIND', 'frontend')
    if kind not in ('frontend', 'ir') or kind != requested_kind:
        raise RuntimeError('Configured FastInterp PGO kind differs from requested policy')
    if generate != requested_generate or use != expected_use:
        raise RuntimeError('Configured FastInterp PGO differs from requested policy')
    if generate and use:
        raise RuntimeError('FastInterp PGO generation and use are mutually exclusive')
    if (generate or use) and (cache.get('CARA_PGO_GENERATE', 'OFF').upper() in ('ON', 'TRUE', 'YES', '1') or cache.get('CARA_PGO_USE', '')):
        raise RuntimeError('FastInterp-only PGO cannot be combined with whole-engine PGO')
    fi = shlex.split(units['fastinterp']['flags'])
    metal = shlex.split(units['metal_bindings']['flags'])
    linked = shlex.split(link['flags'])
    generation_flag = '-fprofile-generate' if kind == 'ir' else '-fprofile-instr-generate'
    use_flag = '-fprofile-use=' if kind == 'ir' else '-fprofile-instr-use='
    generation_prefixes = ('-fprofile-instr-generate', '-fprofile-generate')
    use_prefixes = ('-fprofile-instr-use=', '-fprofile-use=')
    expected_generation = [generation_flag] if generate else []
    if ([flag for flag in fi if flag.startswith(generation_prefixes)] != expected_generation or
            [flag for flag in linked if flag.startswith(generation_prefixes)] != expected_generation or
            any(flag.startswith(('-fno-profile-instr', '-fno-profile-generate', '-fno-profile-use')) for flag in (*fi, *linked))):
        raise RuntimeError('FastInterp PGO generation compile/link runtime flags differ from requested policy')
    use_flags = [flag for flag in fi if flag.startswith(use_prefixes)]
    if (generation_flag in fi) != generate or (generation_flag in linked) != generate:
        raise RuntimeError('FastInterp PGO generation compile/link runtime flags differ from requested policy')
    if use_flags != ([f'{use_flag}{use}'] if use else []):
        raise RuntimeError('FastInterp PGO use flags differ from requested profile')
    if any(flag.startswith((*generation_prefixes, *use_prefixes)) for flag in metal):
        raise RuntimeError('FastInterp-only PGO unexpectedly instruments the Metal bridge')
    report = {'mode': 'generate' if generate else 'use' if use else 'off',
              'kind': kind,
              'profile_kind_verified': None,
              'profile_path': use or None, 'profile_sha256': None,
              'instrumented_compile_unit': units['fastinterp']['output'] if generate else None,
              'generation_runtime_linked': generation_flag in linked,
              'compiler': cache.get('CMAKE_CXX_COMPILER'),
              'profile_runtime_output': 'Set LLVM_PROFILE_FILE to an isolated path, preferably including %p',
              'scope': 'Only fastinterp.cpp; defaults and guest semantics unchanged'}
    if use:
        profile = Path(use)
        if not profile.is_file():
            raise RuntimeError('FastInterp PGO profile is not an existing file')
        if not {'-Werror=profile-instr-out-of-date', '-Werror=profile-instr-unprofiled'}.issubset(fi):
            raise RuntimeError('FastInterp PGO requires exact profile mismatch diagnostics')
        actual_kind = profile_instrumentation_kind(profile, cache.get('CMAKE_CXX_COMPILER'))
        if actual_kind != kind:
            raise RuntimeError('Supplied LLVM profile instrumentation kind differs from requested FastInterp PGO kind')
        report['profile_kind_verified'] = actual_kind
        report['profile_sha256'] = hashlib.sha256(profile.read_bytes()).hexdigest()
    return report


def audit_compiler_policy(build, target, mac, requested_lto, fastinterp_pgo_generate=False, fastinterp_pgo_use=None, fastinterp_pgo_kind='frontend'):
    """Check configured CMake policy against actual generated compile/link flags.

    CMake's IPO switch can omit ObjC++, so require actual ThinLTO in both
    audited engine languages and the link recipe when --lto is requested.
    """
    cache = read_cmake_cache(build)
    value = cache.get('CARA_ENABLE_LTO', '').upper()
    if value not in ('ON', 'OFF', 'TRUE', 'FALSE', 'YES', 'NO', '1', '0'):
        raise RuntimeError('Missing or invalid CARA_ENABLE_LTO in CMakeCache.txt')
    configured_lto = value in ('ON', 'TRUE', 'YES', '1')
    if configured_lto != requested_lto:
        raise RuntimeError('Configured LTO differs from requested native Metal build policy')
    if cache.get('CARA_CPU_TARGET') != 'generic':
        raise RuntimeError('The native Metal builder requires the portable generic CPU target')
    graph = (build / 'build.ninja').read_text()
    units = {}
    for name, output in (
        ('fastinterp', 'src/core/CMakeFiles/core.dir/arm/fastinterp/fastinterp.cpp.o'),
        ('metal_bindings', 'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_native_bindings.mm.o'),
    ):
        fields = ninja_build_fields(graph, output)
        flags = fields.get('FLAGS', '')
        units[name] = {'output': output, 'flags': flags, 'lto_mode': lto_mode(flags)}
    binary = f'bin/{target}.app/' + (f'Contents/MacOS/{target}' if mac else target)
    fields = ninja_build_fields(graph, binary)
    flags = ' '.join(fields.get(name, '') for name in ('FLAGS', 'LINK_FLAGS')).strip()
    link = {'output': binary, 'flags': flags, 'lto_mode': lto_mode(flags)}
    if bool(units['fastinterp']['lto_mode']) != requested_lto:
        raise RuntimeError('FastInterp compile flags differ from requested LTO policy')
    if bool(link['lto_mode']) != requested_lto:
        raise RuntimeError('Executable link flags differ from requested LTO policy')
    if bool(units['metal_bindings']['lto_mode']) != requested_lto:
        raise RuntimeError('Metal ObjC++ compile flags differ from requested LTO policy')
    if requested_lto and any(stage['lto_mode'] != 'thin' for stage in (*units.values(), link)):
        raise RuntimeError('The native Metal LTO policy requires ThinLTO in C++, ObjC++ and executable link flags')
    report = {
        'cpu_target': cache['CARA_CPU_TARGET'],
        'lto_requested': requested_lto,
        'lto_configured': configured_lto,
        'compile_units': units,
        'executable_link': link,
        'all_audited_compile_units_lto': all(bool(unit['lto_mode']) for unit in units.values()),
        'fastinterp_pgo': audit_fastinterp_pgo(build, units, link, fastinterp_pgo_generate, fastinterp_pgo_use, fastinterp_pgo_kind),
    }
    (build / 'native-metal-compiler-policy.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def bundle_and_audit(build, target, mac, cpu_jit, deps, lto=False, fastinterp_pgo_generate=False, fastinterp_pgo_use=None, fastinterp_pgo_kind='frontend'):
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

    report = {
        'source_revision': read(['git', 'rev-parse', 'HEAD'], cwd=ROOT),
        'tracked_source_modified': bool(read(
            ['git', 'status', '--porcelain', '--untracked-files=no', '--', 'emulator'], cwd=ROOT)),
        'renderer': 'native_metal',
        'architecture': 'arm64',
        'platform': 'mac' if mac else 'ios',
        'cpu_jit_enabled': cpu_jit,
        'compiler_policy': audit_compiler_policy(build, target, mac, lto, fastinterp_pgo_generate, fastinterp_pgo_use, fastinterp_pgo_kind),
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
    parser.add_argument('--lto', action='store_true',
                        help='Enable the existing CMake link-time optimization policy (default: off)')
    pgo = parser.add_mutually_exclusive_group()
    pgo.add_argument('--fastinterp-pgo-generate', action='store_true',
                     help='Instrument only fastinterp.cpp; set an isolated LLVM_PROFILE_FILE when training')
    pgo.add_argument('--fastinterp-pgo-use', type=Path,
                     help='Use an exact profile of the selected kind merged with this compiler toolchain llvm-profdata')
    parser.add_argument('--fastinterp-pgo-kind', choices=('frontend', 'ir'), default='frontend',
                        help='Private FI-only PGO instrumentation kind (default: frontend)')
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
    base_command = configure_args(args, build, deps, cmake, ninja)
    def configure(override):
        command = list(base_command)
        if override is not None:
            command.append('-DCMAKE_OBJCXX_FLAGS_RELEASE:STRING=' + override[0])
        (build / 'native-metal-configure.json').write_text(json.dumps(command, indent=2) + '\n')
        run(command)
        commit_objcxx_lto_override(build, override)
    configure(objcxx_lto_override(build, args.lto))
    if args.lto:
        graph = (build / 'build.ninja').read_text()
        fields = ninja_build_fields(graph,
            'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_native_bindings.mm.o')
        if lto_mode(fields.get('FLAGS', '')) != 'thin':
            # A fresh build has no cached Release defaults until CMake's first
            # configure. Add only the missing ObjC++ ThinLTO driver option.
            configure(objcxx_lto_override(build, True))
    target = 'cara-metal-rom' if args.platform == 'mac' else 'cara-ios'
    audit_compiler_policy(build, target, args.platform == 'mac', args.lto, args.fastinterp_pgo_generate, args.fastinterp_pgo_use, args.fastinterp_pgo_kind)
    if args.configure_only:
        return
    run([cmake, '--build', str(build), '--target', target, '--parallel', str(args.jobs)])
    bundle_and_audit(build, target, args.platform == 'mac', args.cpu_jit, deps, args.lto, args.fastinterp_pgo_generate, args.fastinterp_pgo_use, args.fastinterp_pgo_kind)


if __name__ == '__main__':
    main()
