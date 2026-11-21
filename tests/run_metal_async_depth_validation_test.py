#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Link the depth fixture against a completed native Metal Mac build."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess


def ninja_fields(graph, target):
    block = re.search(r'^build ' + re.escape(target) + r':[^\n]*\n((?:  [^\n]*\n)*)',
                      graph, re.M)
    if not block:
        raise RuntimeError(f'Missing Ninja target: {target}')
    return dict(re.findall(r'^  ([A-Z_]+) = (.*)$', block[1], re.M))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    graph = (build / 'build.ninja').read_text()
    lane = ninja_fields(graph,
        'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_native_draw_lane.mm.o')
    app = ninja_fields(graph, 'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    flags = sum((shlex.split(lane[key]) for key in ('DEFINES', 'FLAGS', 'INCLUDES')), [])
    env = os.environ.copy()
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'],
                                  env=env, text=True).strip()
    source = Path(__file__).with_name('metal_async_depth_validation_test.mm').resolve()
    obj = build / 'async-depth-test.o'
    binary = build / 'async-depth-test'
    subprocess.run(['xcrun', 'clang++', *flags, '-c', str(source), '-o', str(obj)],
                   cwd=build, env=env, check=True)
    subprocess.run(['xcrun', 'clang++', '-isysroot', sdk, '-mmacosx-version-min=13.0',
                    '-Wl,-dead_strip', str(obj), *shlex.split(app['LINK_LIBRARIES']),
                    '-o', str(binary)], cwd=build, env=env, check=True)
    if args.compile_only:
        return
    for async_import, pending, window in ((0, 0, 8), (1, 0, 8), (1, 1, 8),
                                         (1, 0, 32), (1, 1, 16), (1, 1, 32)):
        child = {k: v for k, v in env.items() if not k.startswith('CARA_')}
        child.update(CARA_METAL_ASYNC_CHECKED_DEPTH_IMPORT=str(async_import),
                     CARA_METAL_QUEUE_PENDING_DEPTH_EXPORT=str(pending),
                     CARA_METAL_SUBMISSION_WINDOW=str(window), MTL_DEBUG_LAYER='1')
        subprocess.run([str(binary), str(build / 'src/video_core/cara-pica.metallib')],
                       cwd=build, env=child, check=True, timeout=30)


if __name__ == '__main__':
    main()
