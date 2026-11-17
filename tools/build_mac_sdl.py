#!/usr/bin/env python3
"""Build the separate native macOS SDL developer host; leave probe defaults alone."""
import argparse
import hashlib
import time
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def audit(build):
    cache = (build / 'CMakeCache.txt').read_text()
    if 'CARA_MAC_SDL_DEV_HOST:BOOL=ON' not in cache:
        raise RuntimeError('Explicit native Mac frontend must be selected')
    if 'CARA_ENABLE_JIT:BOOL=OFF' not in cache:
        raise RuntimeError('Global CPU/shader JIT flag must stay disabled')
    cpu_jit = 'CARA_MAC_CPU_JIT:BOOL=ON' in cache
    graph = (build / 'build.ninja').read_text()
    forbidden_objects = [name for name in ('shader/shader_jit_x64.cpp.o',) if name in graph]
    if not cpu_jit and 'arm/dynarmic/arm_dynarmic.cpp.o' in graph:
        forbidden_objects.append('arm/dynarmic/arm_dynarmic.cpp.o')
    if cpu_jit and 'arm/dynarmic/arm_dynarmic.cpp.o' not in graph:
        raise RuntimeError('Requested CPU JIT object missing')
    binary = build / 'bin/cara'
    symbols = subprocess.check_output(['nm', str(binary)], text=True)
    # Atlas diagnostics map two file-backed journals as RW, never executable.
    # Keep executable-memory/JIT rejection; permit mmap only for the explicit
    # atlas build after checking the actual compiled observer source calls.
    atlas = 'CARA_MAC_ATLAS_PIPELINE:BOOL=ON' in cache
    journal_maps = []
    if atlas:
        for name in ('gl_host_latency.cpp', 'gl_pipeline_capture.cpp'):
            source = ROOT / 'emulator/src/video_core/renderer_opengl' / name
            if name + '.o' not in graph:
                raise RuntimeError('Missing compiled journal observer: ' + name)
            calls = re.findall(r'\bmmap\s*\([^;]*', source.read_text())
            expected = 'mmap(nullptr,sizeof(Journal),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0)'
            if len(calls) != 1 or re.sub(r'\s+', '', calls[0]) != expected:
                raise RuntimeError('Unreviewed memory mapping in ' + name)
            journal_maps.append(name)
    forbidden_symbols = [line for line in symbols.splitlines()
                         if re.search(r'JitX64Engine', line)
                         or (not cpu_jit and (re.search(r'ARM_Dynarmic| _mprotect$| _vm_protect$', line)
                         or (not atlas and re.search(r' _mmap$', line))))]
    if cpu_jit and not any('ARM_Dynarmic' in line for line in symbols.splitlines()):
        raise RuntimeError('Requested CPU JIT symbols missing')
    if forbidden_objects or forbidden_symbols:
        raise RuntimeError('Excluded JIT facilities found: ' + repr(forbidden_objects + forbidden_symbols))
    # SDL2/SDL3 and libpng are expected dynamic developer dependencies. This is
    # deliberately separate from the system-libraries-only iOS product audit.
    report = dict(source_revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(), binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), platform='mac-sdl', jit_enabled=cpu_jit, shader_jit_enabled=False, default_cpu='fastinterp', forbidden_objects=forbidden_objects,
                  forbidden_symbols=forbidden_symbols,
                  non_executable_journal_maps=journal_maps,
                  linkage=subprocess.check_output(['otool', '-L', str(binary)], text=True))
    (build / ('native-cpu-jit-audit.json' if cpu_jit else 'native-no-jit-audit.json')).write_text(json.dumps(report, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'work/build-mac-sdl')
    parser.add_argument('--cmake', default=shutil.which('cmake'))
    parser.add_argument('--ninja', default=shutil.which('ninja'))
    parser.add_argument('--sdl-prefix', type=Path)
    parser.add_argument('--prefix-path', type=Path, help='Additional CMake dependency prefix, e.g. Homebrew')
    parser.add_argument('--jobs', type=int, choices=(1, 2), default=2)
    parser.add_argument('--cpu-jit', action='store_true', help='Include the experimental M1 Dynarmic CPU backend; FastInterp remains default')
    parser.add_argument('--lto', action='store_true')
    parser.add_argument('--atlas', action='store_true', help='Build the explicit native GL4.1 atlas/render-queue developer path')
    parser.add_argument('--compact-decoded', action='store_true', help='Opt in to experimental 16-byte interpreter records')
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('This helper targets native macOS; use the existing build helpers elsewhere')
    if not args.cmake or not args.ninja:
        parser.error('Provide CMake and Ninja through PATH or --cmake/--ninja')
    prefix = args.sdl_prefix
    if prefix is None:
        sdl_config = shutil.which('sdl2-config')
        if sdl_config:
            prefix = Path(subprocess.check_output([sdl_config, '--prefix'], text=True).strip())
        else:
            prefix = next((p for p in (Path('/opt/homebrew/opt/sdl2'), Path('/usr/local/opt/sdl2'))
                           if p.is_dir()), None)
    if prefix is None or not (prefix / 'include/SDL2/SDL.h').is_file():
        parser.error('SDL2 development headers missing; supply --sdl-prefix')
    library = prefix / 'lib/libSDL2.dylib'
    if not library.is_file():
        parser.error('SDL2 dylib missing under ' + str(prefix))
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    temporary = build / 'tmp'
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env['TMPDIR'] = str(temporary)
    prefixes = [str(prefix)]
    if args.prefix_path:
        prefixes.append(str(args.prefix_path.resolve()))
    else:
        prefixes.extend(str(p) for p in (Path('/opt/homebrew'), Path('/usr/local')) if p.is_dir())
    command = [args.cmake, '-S', str(ROOT / 'emulator'), '-B', str(build), '-G', 'Ninja',
               '-DCMAKE_MAKE_PROGRAM=' + args.ninja, '-DCMAKE_BUILD_TYPE=Release',
               '-DCMAKE_OSX_ARCHITECTURES=' + platform.machine(),
               '-DCMAKE_PREFIX_PATH=' + ';'.join(prefixes),
               '-DCARA_ENABLE_JIT=OFF', '-DCARA_MAC_CPU_JIT=' + ('ON' if args.cpu_jit else 'OFF'), '-DCARA_CPU_TARGET=generic',
               '-DCARA_ENABLE_LTO=' + ('ON' if args.lto else 'OFF'),
               '-DCARA_FASTINTERP_COMPACT_DECODED=' + ('ON' if args.compact_decoded else 'OFF'),
               '-DCARA_SOFTWARE_PRESENTATION=OFF', '-DCARA_INTERPRETER_GUEST_DEBUG=OFF',
               '-DCARA_PGO_GENERATE=OFF', '-DCARA_PGO_USE=',
               '-DCARA_MAC_ATLAS_PIPELINE=' + ('ON' if args.atlas else 'OFF'),
               '-DCARA_MAC_SDL_DEV_HOST=ON', '-DENABLE_SDL2=ON', '-DENABLE_QT=OFF', '-DENABLE_CUBEB=OFF',
               '-DENABLE_WEB_SERVICE=OFF', '-DENABLE_FFMPEG_AUDIO_DECODER=OFF',
               '-DENABLE_FFMPEG_VIDEO_DUMPER=OFF',
               '-DSDL2_INCLUDE_DIR=' + str(prefix / 'include/SDL2'),
               '-DSDL2_LIBRARY_TEMP=' + str(library),
               '-DBOOST_ROOT=' + str(ROOT / 'emulator/externals/boost'),
               '-DBoost_NO_SYSTEM_PATHS=ON', '-DBoost_NO_BOOST_CMAKE=ON']
    main_library = prefix / 'lib/libSDL2main.a'
    if main_library.exists():
        command.append('-DSDL2MAIN_LIBRARY=' + str(main_library))
    version = subprocess.check_output([args.cmake, '--version'], text=True).splitlines()[0]
    if int(re.search(r'version (\d+)', version).group(1)) >= 4:
        command.append('-DCMAKE_POLICY_VERSION_MINIMUM=3.5')
    (build / 'configure-command.json').write_text(json.dumps(command, indent=2) + '\n')
    progress=build/'native-build-in-progress.json'
    if progress.exists():
        shutil.copy2(progress, build/('native-incomplete-'+str(time.time_ns())+'.json'))
    revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    progress.write_text(json.dumps(dict(source_revision=revision,command=command),indent=2)+'\n')
    subprocess.run(command, check=True, env=env)
    subprocess.run([args.cmake, '--build', str(build), '--target', 'cara', '-j' + str(args.jobs)],
                   check=True, env=env)
    if subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()!=revision:
        raise RuntimeError('Source revision changed during the build')
    audit(build)
    progress.unlink()
    print(build / 'bin/cara')


if __name__ == '__main__':
    main()
