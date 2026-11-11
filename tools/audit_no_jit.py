#!/usr/bin/env python3
"""Fail if the linked product or build graph contains excluded JIT facilities."""
import argparse
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('platform', choices=['mac', 'ios', 'simulator'])
parser.add_argument('--renderer', choices=['software', 'gles3'], default='software')
parser.add_argument('--build-dir', type=Path)
args = parser.parse_args()
build = ROOT / 'work' / ('build-' + args.platform + ('-gles3' if args.renderer == 'gles3' else ''))
if args.build_dir:
    build = args.build_dir.resolve()
binary = build / 'bin' / ('cara-no-jit-probe' if args.platform == 'mac' else 'cara-ios.app/cara-ios')
# Stale platform guards can compile but select desktop GL requirements on iOS.
obsolete = re.compile(rb'\bCITRA_(?:IOS(?:_GLES|_HARDWARE_RENDERER)?|ENABLE_JIT|SOFTWARE_PRESENTATION|INTERPRETER_GUEST_DEBUG|NO_GUEST_DEBUGGER)\b')
source_files = [p for p in (ROOT / 'emulator/src').rglob('*') if p.suffix in ('.cpp', '.h', '.mm', '.txt')]
source_files.append(ROOT / 'emulator/externals/CMakeLists.txt')
for source in source_files:
    assert not obsolete.search(source.read_bytes()), 'Obsolete build switch in ' + str(source)
cache = (build / 'CMakeCache.txt').read_text()
assert 'CARA_ENABLE_JIT:BOOL=OFF' in cache, 'JIT is not disabled in the cache'
assert ('CARA_SOFTWARE_PRESENTATION:BOOL=' + ('ON' if args.renderer == 'software' else 'OFF')) in cache
commands = (build / 'build.ninja').read_text()
for excluded in ['arm/dynarmic/arm_dynarmic.cpp.o', 'shader/shader_jit_x64.cpp.o']:
    assert excluded not in commands, 'Unexpected JIT object: ' + excluded
symbols = subprocess.check_output(['nm', str(binary)], text=True)
forbidden = re.findall(r'^.*(?:ARM_Dynarmic|JitX64Engine| _mmap$| _mprotect$| _vm_protect$).*$', symbols, re.M)
assert not forbidden, 'Unexpected JIT/executable-memory symbols: ' + repr(forbidden)
libraries = subprocess.check_output(['otool', '-L', str(binary)], text=True)
for line in libraries.splitlines()[1:]:
    lib = line.strip().split(' (')[0]
    assert lib.startswith(('/System/Library/', '/usr/lib/')), 'Non-system dynamic dependency: ' + lib
report = {'platform': args.platform, 'jit_enabled': False, 'software_presentation': args.renderer == 'software', 'renderer': args.renderer,
          'forbidden_symbols': forbidden, 'linkage': 'static emulator; Apple system libraries dynamic'}
(build / 'no-jit-audit.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
