#!/usr/bin/env python3
"""Build/run the color authority-lease reservation GPU regression against an existing native Metal build.

Usage: python3 tests/run_metal_color_lease_reservation_test.py [--build-dir PATH]
No CMake rebuild, downloads, ROM, or private iOS diagnostic flags are needed.
The lane is compiled directly; other engine code and the real Metal
library come from the supplied build. Run only outside timing benchmarks.
API validation stays enabled. Legacy authority-byte export has a separately
recorded GPU-shader-validation defect, so this ordering fixture disables only
that validator; it does not claim shader-validation coverage.
"""
import argparse
import json
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
    parser.add_argument('--retire-at-record', action='store_true')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    ninja = (build/'build.ninja').read_text()
    lane = ninja_block(ninja,
        'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_native_draw_lane.mm.o')
    link = ninja_block(ninja, 'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    output = root/'work/portable-metal-checks/color-lease-reservation'
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
    run([*command,'-c',str(root/'emulator/src/video_core/renderer_metal/pica_metal_native_draw_lane.mm'),
         '-o',str(output/'lane.o')])
    run([*command,'-Wextra','-Werror','-Wno-unused-parameter','-Wno-deprecated-copy','-Wno-switch','-c',str(root/'tests/metal_color_lease_reservation_test.mm'),
         '-o',str(output/'test.o')])
    binary = output/'color-lease-reservation-test'
    run([compiler,'-mmacosx-version-min=13.0','-Wl,-dead_strip',str(output/'test.o'),str(output/'lane.o'),
         *shlex.split(link['LINK_LIBRARIES']),'-o',str(binary)])
    if args.compile_only:
        print(f'Built {binary}')
        return
    library = build/'src/video_core/cara-pica.metallib'
    results=[]
    for option,late in (('0','0'),('1','0'),('0','1'),('1','1')):
        child_env={name:value for name,value in env.items() if not name.startswith('CARA_')}
        child_env.update(CARA_METAL_COLOR_LEASE_NO_RESERVE=option,
                         CARA_METAL_LATE_DRAW_RESERVE=late,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='0')
        child_env['CARA_METAL_RETIRE_CACHED_INPUTS_AT_RECORD'] = str(int(args.retire_at_record))
        result=subprocess.run([str(binary),str(library)],env=child_env,cwd=root,text=True,capture_output=True,timeout=20)
        results.append(dict(color_lease=option,late_record=late,retire_at_record=args.retire_at_record,exit_code=result.returncode,
                            stdout=result.stdout,stderr=result.stderr))
        (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(result.stdout,end='');print(result.stderr,end='');result.check_returncode()



if __name__ == '__main__':
    main()
