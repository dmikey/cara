#!/usr/bin/env python3
"""Compile and GPU-check tracked/untracked production upload sources.

Run only outside timing measurements. No public header, guest state, ROM,
shader dump, or dependency download is required.
"""
import argparse
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
    block = ninja_block(ninja, 'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_upload.mm.o')
    link = ninja_block(ninja, 'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    flags = shlex.split(block['DEFINES']) + shlex.split(block['FLAGS'])
    for item in shlex.split(block['INCLUDES']):
        flags += ['-isystem', item[2:]] if item.startswith('-I') and '/externals/' in item else [item]
    flags += ['-Wno-deprecated-declarations', '-Wno-deprecated-builtins', '-Wno-return-type']
    env = dict(os.environ)
    env.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    compiler = subprocess.check_output(['xcrun', '--find', 'clang++'], env=env, text=True).strip()
    output = root/'work/portable-metal-checks/untracked-upload'
    output.mkdir(parents=True, exist_ok=True)
    def run(argv):
        subprocess.run(argv, cwd=build, env=env, check=True)
    upload = output/'upload.o'
    run([compiler, *flags, '-c', str(root/'emulator/src/video_core/renderer_metal/pica_metal_upload.mm'), '-o', str(upload)])
    binaries = []
    for name in ('metal_untracked_upload_test',):
        obj = output/(name+'.o')
        binary = output/name
        run([compiler, *flags, '-c', str(root/'tests'/(name+'.mm')), '-o', str(obj)])
        # Link the production override before the unchanged engine archive.
        overrides = [str(upload)]
        run([compiler, '-mmacosx-version-min=13.0', '-Wl,-dead_strip', str(obj), *overrides,
             *shlex.split(link['LINK_LIBRARIES']), '-o', str(binary)])
        binaries.append(binary)
    if args.compile_only:
        print('Built', *binaries)
        return
    results = []
    for enabled in ('0', '1'):
        for reuse in ('0', '1'):
            child = {k: v for k, v in env.items() if not k.startswith('CARA_')}
            child.update(CARA_METAL_UNTRACKED_UPLOAD_SOURCES=enabled,
                         CARA_METAL_DRAW_INPUT_REUSE=reuse, MTL_DEBUG_LAYER='1')
            for binary in binaries:
                result = subprocess.run([str(binary)], env=child, text=True, capture_output=True)
                results.append(dict(untracked=enabled, reuse=reuse, exit_code=result.returncode,
                                    stdout=result.stdout, stderr=result.stderr))
                (output/'results.json').write_text(json.dumps(results, indent=2)+'\n')
                print(result.stdout, end='')
                print(result.stderr, end='')
                if result.returncode:
                    raise RuntimeError('Untracked upload regression failed; see results.json')


if __name__ == '__main__':
    main()
