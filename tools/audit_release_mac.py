#!/usr/bin/env python3
"""Validate portable linkage and run the native launcher without developer tools."""
import argparse
import hashlib
import json
import os
import plistlib
from pathlib import Path
import subprocess
import tempfile
from mac_bundle import audit, version_key


def verify(app):
    app = app.resolve()
    resources = app / 'Contents/Resources'
    manifest = json.loads((resources / 'build.json').read_text())
    binary = app / 'Contents/MacOS/cara-core'
    if manifest.get('self_contained') is not True:
        raise RuntimeError('Not a self-contained package')
    if hashlib.sha256(binary.read_bytes()).hexdigest() != manifest['binary_sha256']:
        raise RuntimeError('Packaged core differs from its manifest')
    binaries = audit(app)
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    if any(version_key(b['minimum_os']) > version_key(info['LSMinimumSystemVersion']) for b in binaries):
        raise RuntimeError('App declares support for an older macOS than its bundled binaries')
    compat = any(b'@loader_path/libSDL3.dylib' in p.read_bytes()
                 for p in (app / 'Contents/Frameworks').glob('libSDL2*.dylib'))
    if compat and not (app / 'Contents/Frameworks/libSDL3.dylib').is_file():
        raise RuntimeError('Missing runtime-loaded SDL3')
    subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)], check=True)
    with tempfile.TemporaryDirectory() as home:
        # Invoke a Mach-O launcher; no Python, xcrun, Homebrew, or developer PATH at runtime.
        env = {'HOME': home, 'PATH': '/usr/bin:/bin:/usr/sbin:/sbin',
               'TMPDIR': home, 'DYLD_PRINT_LIBRARIES': '1'}
        result = subprocess.run([str(app / 'Contents/MacOS/Cara'), '--verify-package'],
                                env=env, text=True, capture_output=True, timeout=20, check=True)
        if list(Path(home).iterdir()):
            raise RuntimeError('Package verification unexpectedly wrote user data')
    if any(path in result.stderr for path in ('/opt/homebrew/', '/usr/local/', '/Library/Developer/', '/Applications/Xcode')):
        raise RuntimeError('Runtime escaped the bundle to a development dependency')
    loaded = result.stderr
    for dependency in (app / 'Contents/Frameworks').glob('*.dylib'):
        if dependency.name not in loaded:
            raise RuntimeError('Bundled dependency was not actually loaded: ' + dependency.name)
    return dict(passed=True, platform='macOS-arm64', self_contained=True,
                native_launcher=True, jit_available=manifest['cpu_jit_available'],
                shader_jit=False, game_launched=False, clean_home=True,
                external_developer_runtime_dependencies=False,
                binaries=binaries, loader_output=loaded)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = verify(args.app)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print('Portable Mac package audit PASS')


if __name__ == '__main__':
    main()
