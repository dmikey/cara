#!/usr/bin/env python3
"""Package the local Mac build with Settings and an empty Welcome screen."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--user-dir', default='~/Library/Application Support/Cara')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    output = args.output.resolve()
    if output.exists():
        parser.error('Output exists; preserve it or choose another path')
    cache = (build / 'CMakeCache.txt').read_text()
    cpu_jit = 'CARA_MAC_CPU_JIT:BOOL=ON' in cache
    audit_name = 'native-cpu-jit-audit.json' if cpu_jit else 'native-no-jit-audit.json'
    audit = json.loads((build / audit_name).read_text())
    if audit.get('binary_sha256') != hashlib.sha256((build / 'bin/cara').read_bytes()).hexdigest():
        parser.error('Build binary differs from its audit')
    if audit.get('forbidden_objects') or audit.get('forbidden_symbols'):
        parser.error('Build audit failed')
    contents = output / 'Contents'
    resources = contents / 'Resources'
    macos = contents / 'MacOS'
    resources.mkdir(parents=True)
    macos.mkdir()
    binary = macos / 'cara-core'
    shutil.copy2(build / 'bin/cara', binary)
    shutil.copy2(ROOT / 'tools/mac_app_launcher.py', resources / 'launch.py')
    shutil.copy2(ROOT / 'emulator/dist/cara.icns', resources / 'cara.icns')
    # The entitlement is specific to the optional CPU backend, never the shader renderer.
    entitlements = resources / 'entitlements.plist'
    entitlements.write_bytes(plistlib.dumps(
        {'com.apple.security.cs.allow-jit': True} if cpu_jit else {}))
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--entitlements',
                    str(entitlements), str(binary)], check=True)
    manifest = dict(binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    user_dir=args.user_dir, cpu_jit_available=cpu_jit,
                    cpu_default='fastinterp', shader_jit=False, automatic_game=False,
                    source_revision=subprocess.check_output(
                        ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip())
    (resources / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    shutil.copy2(build / audit_name, resources / audit_name)
    launcher = macos / 'Cara'
    launcher.write_text('#!/bin/zsh\nexec /Library/Developer/CommandLineTools/usr/bin/python3 "${0:A:h:h}/Resources/launch.py" "$@"\n')
    launcher.chmod(0o755)
    info = dict(CFBundleName='Cara', CFBundleDisplayName='Cara', CFBundleExecutable='Cara',
                CFBundleIdentifier='org.cara.mac', CFBundlePackageType='APPL',
                CFBundleIconFile='cara.icns', CFBundleShortVersionString='0.2',
                CFBundleVersion='20261117.1', LSMinimumSystemVersion='13.0',
                NSHighResolutionCapable=True)
    (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(output)], check=True)
    subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(output)], check=True)
    print(output)


if __name__ == '__main__':
    main()
