#!/usr/bin/env python3
"""Build/run the presentation receipt visibility regression against an existing native Metal build.

Usage: python3 tests/run_metal_presentation_validation_test.py [--build-dir PATH]
No CMake rebuild, downloads, ROM, or private iOS diagnostic flags are needed.
The actual lane implementation is included in the fixture; its final LCD
call is intercepted to observe visibility gating without showing pixels.
Other engine code and the real depth kernels come from the supplied build. Run only outside timing benchmarks.
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
    output = root/'work/portable-metal-checks/presentation-validation'
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
    # The fixture includes the actual lane implementation and intercepts only
    # its final LCD call, so no separate lane override is linked.
    run([*command,'-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function',
         '-Wno-deprecated-declarations','-Wno-deprecated-copy','-Wno-switch',
         '-c',str(root/'tests/metal_presentation_validation_test.mm'),'-o',str(output/'test.o')])
    binary = output/'presentation-validation-test'
    run([compiler,'-mmacosx-version-min=13.0','-Wl,-dead_strip',str(output/'test.o'),
         *shlex.split(link['LINK_LIBRARIES']),'-o',str(binary)])
    if args.compile_only:
        print(f'Built {binary}')
        return
    library = build/'src/video_core/cara-pica.metallib'
    child_env={name:value for name,value in env.items() if not name.startswith('CARA_')}
    child_env.update(MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='0')
    result=subprocess.run([str(binary),str(library)],env=child_env,cwd=root,text=True,capture_output=True,timeout=30)
    (output/'validation.log').write_text(result.stdout+result.stderr)
    print(result.stdout,end='');print(result.stderr,end='');result.check_returncode()



if __name__ == '__main__':
    main()
