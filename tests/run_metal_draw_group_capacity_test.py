#!/usr/bin/env python3
"""Run production grouped-render capacity/lifetime checks against a freshly rebuilt Metal archive."""
import argparse
import csv
import json
import os
from pathlib import Path
import shlex
import subprocess
from run_metal_texture_copy_batch_test import ninja_block


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=root/'work/build-native-metal-mac')
    parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    ninja = (build/'build.ninja').read_text()
    block = ninja_block(ninja, 'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_draw_group.mm.o')
    link = ninja_block(ninja, 'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    env = dict(os.environ)
    env.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    compiler = subprocess.check_output(['xcrun', '--find', 'clang++'], env=env, text=True).strip()
    flags = shlex.split(block['DEFINES']) + shlex.split(block['FLAGS'])
    for item in shlex.split(block['INCLUDES']):
        flags += ['-isystem', item[2:]] if item.startswith('-I') and '/externals/' in item else [item]
    output = root/'work/portable-metal-checks/draw-group-capacity'
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([compiler, *flags, '-Wextra', '-Werror', '-c', str(root/'tests/metal_draw_group_capacity_test.mm'),
                    '-o', str(output/'test.o')], cwd=build, env=env, check=True)
    binary = output/'draw-group-capacity-test'
    subprocess.run([compiler, '-mmacosx-version-min=13.0', '-Wl,-dead_strip', str(output/'test.o'),
                    *shlex.split(link['LINK_LIBRARIES']), '-o', str(binary)], cwd=build, env=env, check=True)
    if args.compile_only:
        print('Built', binary)
        return
    results = []
    for limit in (32, 64, 128, 256):
        child = {k:v for k,v in env.items() if not k.startswith('CARA_')}
        child.update(CARA_METAL_DRAW_GROUP_LIMIT=str(limit), MTL_DEBUG_LAYER='1')
        receipt = output/('limit-'+str(limit))
        receipt.mkdir(exist_ok=True)
        result = subprocess.run([str(binary), str(receipt)+'/'], cwd=root, env=child, check=True, text=True, capture_output=True)
        info = json.loads((receipt/'metal-command-timeline-info.json').read_text())
        timings = list(csv.DictReader((receipt/'metal-command-timeline.csv').open()))
        if info['retained'] != 2 or info['missing'] or info['dropped'] or len(timings) != 2:
            raise RuntimeError('Incomplete actual-command timing receipt')
        if not all(0 < float(row['start_seconds']) <= float(row['end_seconds']) for row in timings):
            raise RuntimeError('Invalid actual GPU timestamps')
        print(result.stdout.strip())
        results.append(dict(limit=limit, exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr))
    (output/'results.json').write_text(json.dumps(results, indent=2)+'\n')


if __name__ == '__main__':
    main()
