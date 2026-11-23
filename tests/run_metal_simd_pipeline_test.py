#!/usr/bin/env python3
"""Build/run the production SIMD pipeline GPU regression against an existing native Metal build.

Usage: python3 tests/run_metal_simd_pipeline_test.py [--build-dir PATH]
No CMake rebuild, downloads, ROM, or private iOS diagnostic flags are needed.
The test links the supplied engine archives and compiles authored shaders at runtime. Run only outside timing benchmarks.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess


def ninja_block(text, target):
    match = re.search(r'^build ' + re.escape(target) + r':[^\n]*\n((?:  [^\n]*\n)*)', text, re.M)
    if not match:
        raise RuntimeError(f'Missing build target: {target}')
    return dict(re.findall(r'^  ([A-Z_]+) = (.*)$', match[1], re.M))


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=root/'work/build-native-metal-mac')
    parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    ninja = (build/'build.ninja').read_text()
    lane = ninja_block(ninja,
        'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_native_draw_lane.mm.o')
    link = ninja_block(ninja, 'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    output = root/'work/portable-metal-checks/simd-pipeline'
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    compiler = subprocess.check_output(['xcrun', '--find', 'clang++'], env=env, text=True).strip()
    flags = shlex.split(lane['DEFINES']) + shlex.split(lane['FLAGS'])
    includes = shlex.split(lane['INCLUDES'])
    # Keep diagnostics in our code while treating bundled third-party headers
    # the same way as external system dependencies.
    for item in includes:
        if item.startswith('-I') and '/externals/' in item:
            flags += ['-isystem',item[2:]]
        else:
            flags.append(item)
    command = [compiler,*flags]
    def run(argv):
        subprocess.run(argv, cwd=build, env=env, check=True)
    run([*command,'-Wextra','-Werror','-Wno-deprecated-declarations','-Wno-unused-parameter','-Wno-deprecated-copy','-Wno-switch','-c',str(root/'tests/metal_simd_pipeline_test.mm'),
         '-o',str(output/'test.o')])
    binary = output/'simd-pipeline-test'
    run([compiler,'-mmacosx-version-min=13.0','-Wl,-dead_strip',str(output/'test.o'),
         *shlex.split(link['LINK_LIBRARIES']),'-o',str(binary)])
    if args.compile_only:
        print(f'Built {binary}')
        return
    child_env = dict(env)
    for option in ('CARA_RAW_DEPTH', 'CARA_UNSNAPPED_VERTICES'):
        child_env.pop(option, None)
    subprocess.run([str(binary)],env=child_env,cwd=root,check=True)



if __name__ == '__main__':
    main()
