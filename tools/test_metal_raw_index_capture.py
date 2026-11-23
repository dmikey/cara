#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build/run the production raw-index CPU fixture using a configured Metal build.

This links the existing no-JIT engine libraries but creates no Metal device or
GPU work. Build the native Metal Mac target first. Runs four isolated processes
because the production memo/scanner options are process-static.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'work/build-native-metal-mac')
    parser.add_argument('--output', type=Path, default=ROOT / 'work/metal-raw-index-capture-tests')
    args = parser.parse_args()
    build, output = args.build_dir.resolve(), args.output.resolve()
    if not (build / 'build.ninja').is_file():
        parser.error('Build the native Metal Mac target first')
    if output.exists():
        parser.error('Choose a fresh output directory to retain previous results')
    ninja = shutil.which('ninja')
    if not ninja:
        candidate = ROOT / 'work/native-metal-tools/bin/ninja'
        if candidate.is_file():
            ninja = str(candidate)
        else:
            parser.error('Ninja is required to read the configured build recipes')
    recipe = subprocess.check_output(
        [ninja, '-C', str(build), '-t', 'commands', 'cara-fastinterp-self-test'], text=True)
    compile_line = next((line for line in recipe.splitlines()
                         if ' -c ' in line and line.endswith('/fastinterp_self_test.cpp')), None)
    link_line = next((line for line in reversed(recipe.splitlines())
                      if ' -o ' in line and 'bin/cara-fastinterp-self-test' in line), None)
    if not compile_line or not link_line:
        parser.error('Configured FastInterp test compile/link recipes are unavailable')
    output.mkdir(parents=True)
    obj, binary = output / 'raw-index-test.o', output / 'raw-index-test'
    compile_args = shlex.split(compile_line)
    for i, value in enumerate(compile_args):
        if value.endswith('/fastinterp_self_test.cpp'):
            compile_args[i] = str(ROOT / 'tests/metal_raw_index_capture_test.mm')
        if value in ('-o', '-MT'):
            compile_args[i + 1] = str(obj)
        if value == '-MF':
            compile_args[i + 1] = str(obj) + '.d'
    # Match the production renderer's nihstro/GL declarations and Clang setting.
    compile_args += ['-I' + str(ROOT / 'emulator/externals/nihstro/include'),
                     '-I' + str(ROOT / 'emulator/externals/glad/include'),
                     '-Wno-invalid-specialization', '-Wno-deprecated-builtins',
                     '-Wno-deprecated-declarations']
    link_args = shlex.split(link_line)
    if '&&' in link_args:
        first = link_args.index('&&') + 1
        last = link_args.index('&&', first)
        link_args = link_args[first:last]
    link_args = [str(obj) if value.endswith('/fastinterp_self_test.cpp.o') else value
                 for value in link_args]
    link_args[link_args.index('-o') + 1] = str(binary)
    env = {key: value for key, value in os.environ.items() if not key.startswith('CARA_')}
    if 'DEVELOPER_DIR' not in env and Path('/Applications/Xcode.app').is_dir():
        env['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
    with (output / 'build.log').open('w') as log:
        for command in (compile_args, link_args):
            subprocess.run(command, cwd=build, env=env, stdout=log, stderr=subprocess.STDOUT,
                           check=True)
    runs = []
    for memo, specialize in ((0, 0), (1, 0), (0, 1), (1, 1)):
        mode = dict(CARA_METAL_INDEX_SCAN_MEMO=str(memo),
                    CARA_METAL_INDEX_SCAN_SPECIALIZE=str(specialize))
        result = subprocess.run([str(binary)], env=dict(env, **mode), text=True,
                                capture_output=True)
        stem = f'memo-{memo}-specialize-{specialize}'
        (output / (stem + '.stdout')).write_text(result.stdout)
        (output / (stem + '.stderr')).write_text(result.stderr)
        runs.append(dict(options=mode, exit_code=result.returncode,
                         capture_digest=result.stdout.strip(), counters=result.stderr.strip()))
        if result.returncode:
            raise SystemExit(f'Fixture failed; inspect {output / (stem + ".stderr")}')
    if len({run['capture_digest'] for run in runs}) != 1:
        raise SystemExit('Full capture/triangle-oracle digests differ between modes')
    report = dict(binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  compile_command=compile_args, link_command=link_args, runs=runs,
                  scope='CPU capture/index oracle only; no GPU or gameplay speed claim')
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['runs'], indent=2))


if __name__ == '__main__':
    main()
