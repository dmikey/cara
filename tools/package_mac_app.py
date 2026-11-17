#!/usr/bin/env python3
"""Package the local Mac build with Settings and an empty Welcome screen."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import fnmatch
from mac_bundle import bundle, audit as audit_bundle, minimum_os, version_key

ROOT = Path(__file__).resolve().parents[1]


def validate_build_identity(report, binary, revision):
    if report.get('source_revision') != revision:
        raise ValueError('Build audit belongs to another source revision; complete the current build first')
    if report.get('binary_sha256') != hashlib.sha256(binary.read_bytes()).hexdigest():
        raise ValueError('Build binary differs from its audit')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sdl3-library', type=Path, help='SDL3 dylib required by sdl2-compat at runtime')
    parser.add_argument('--user-dir', default='~/Library/Application Support/Cara')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    output = args.output.resolve()
    if output.exists():
        parser.error('Output exists; preserve it or choose another path')
    if (build/'native-build-in-progress.json').exists():
        parser.error('The latest native build did not complete; its previous executable cannot be packaged')
    if subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).strip():
        parser.error('Commit source changes before packaging a release app')
    cache = (build / 'CMakeCache.txt').read_text()
    cpu_jit = 'CARA_MAC_CPU_JIT:BOOL=ON' in cache
    audit_name = 'native-cpu-jit-audit.json' if cpu_jit else 'native-no-jit-audit.json'
    audit = json.loads((build / audit_name).read_text())
    revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    try:
        validate_build_identity(audit, build/'bin/cara', revision)
    except ValueError as error:
        parser.error(str(error))
    if audit.get('jit_enabled') is not cpu_jit or audit.get('shader_jit_enabled') is not False:
        parser.error('Build audit does not match the requested CPU-only JIT policy')
    if audit.get('forbidden_objects') or audit.get('forbidden_symbols'):
        parser.error('Build audit failed')
    contents = output / 'Contents'
    resources = contents / 'Resources'
    macos = contents / 'MacOS'
    resources.mkdir(parents=True)
    macos.mkdir()
    binary = macos / 'cara-core'
    shutil.copy2(build / 'bin/cara', binary)
    shutil.copy2(ROOT / 'tools/mac_runtime.json', resources / 'runtime.json')
    dependencies = bundle(binary, contents / 'Frameworks', args.sdl3_library)
    launcher = macos / 'Cara'
    subprocess.run(['/usr/bin/xcrun', 'clang++', '-std=c++20', '-arch', 'arm64', '-mmacosx-version-min=13.0',
                    '-fobjc-arc', str(ROOT / 'tools/mac_app_launcher.mm'), '-framework', 'AppKit', '-framework', 'Foundation', '-o', str(launcher)], check=True)
    licenses = resources / 'Licenses'
    licenses.mkdir()
    for name in ('LICENSE', 'NOTICE'):
        shutil.copy2(ROOT / name, licenses / name)
    for path in (ROOT / 'emulator/externals').rglob('*'):
        if path.is_file() and any(fnmatch.fnmatch(path.name.lower(), pattern) for pattern in ('license*', 'copying*', 'copyright*')):
            target = licenses / path.relative_to(ROOT / 'emulator')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
    for dependency in dependencies:
        prefix = dependency.parent.parent
        found = [p for p in prefix.glob('*') if p.is_file() and p.name.lower().startswith(('license', 'copying'))]
        for base in (prefix / 'share/licenses', prefix / 'share/doc'):
            if base.is_dir():
                found.extend(p for p in base.rglob('*') if p.is_file() and p.name.lower().startswith(('license', 'copying', 'copyright')))
        if not found:
            raise RuntimeError('Missing bundled dependency license: ' + str(dependency))
        for i, path in enumerate(sorted(set(found))):
            shutil.copy2(path, licenses / (dependency.name + '-' + str(i) + '-' + path.name))
    for library in dependencies.values():
        subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(library)], check=True)
    shutil.copy2(ROOT / 'emulator/dist/cara.icns', resources / 'cara.icns')
    # The entitlement is specific to the optional CPU backend, never the shader renderer.
    entitlements = resources / 'entitlements.plist'
    entitlements.write_bytes(plistlib.dumps(
        {'com.apple.security.cs.allow-jit': True} if cpu_jit else {}))
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--entitlements',
                    str(entitlements), str(binary)], check=True)
    deployment = max([minimum_os(path) for path in [binary, launcher, *dependencies.values()]], key=version_key)
    manifest = dict(minimum_macos=deployment, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    user_dir=args.user_dir, cpu_jit_available=cpu_jit,
                    cpu_default='fastinterp', shader_jit=False, automatic_game=False, self_contained=True,
                    source_url='https://github.com/dmikey/cara',
                    source_revision=revision)
    (resources / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    shutil.copy2(build / audit_name, resources / audit_name)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(launcher)], check=True)
    info = dict(CFBundleName='Cara', CFBundleDisplayName='Cara', CFBundleExecutable='Cara',
                CFBundleIdentifier='org.cara.mac', CFBundlePackageType='APPL',
                CFBundleIconFile='cara.icns', CFBundleShortVersionString='0.2',
                CFBundleVersion='20261117.1', LSMinimumSystemVersion=deployment,
                NSHighResolutionCapable=True)
    (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(output)], check=True)
    subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(output)], check=True)
    (resources / 'bundle-audit.json').write_text(json.dumps(audit_bundle(output), indent=2) + '\n')
    # Include the final audit resource in the outer signature.
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(output)], check=True)
    subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(output)], check=True)
    print(output)


if __name__ == '__main__':
    main()
