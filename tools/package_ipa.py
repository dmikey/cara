#!/usr/bin/env python3
"""Create an unsigned arm64 sideloading IPA from the audited device app."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import stat
import subprocess
import zipfile


def package(app, audit, destination, expected_revision=None):
    app = app.resolve()
    destination = destination.resolve()
    if destination.exists():
        raise ValueError('Output exists; choose a fresh IPA path')
    report = json.loads(audit.read_text())
    if report.get('platform') != 'ios' or report.get('jit_enabled') is not False:
        raise ValueError('An audited no-JIT physical-device iOS build is required')
    if expected_revision is not None and report.get('source_revision') != expected_revision:
        raise ValueError('iOS audit belongs to another source revision')
    info = plistlib.loads((app / 'Info.plist').read_bytes())
    name = info['CFBundleExecutable']
    if Path(name).name != name or not (app / name).is_file():
        raise ValueError('Invalid app executable')
    if not (app / name).stat().st_mode & stat.S_IXUSR:
        raise ValueError('App executable is not executable')
    if report.get('binary_sha256') != hashlib.sha256((app / name).read_bytes()).hexdigest():
        raise ValueError('App executable differs from its no-JIT audit')
    if (app / 'embedded.mobileprovision').exists():
        raise ValueError('Unsigned release packaging does not publish provisioning profiles')
    paths = sorted(app.rglob('*'))
    if any(path.is_symlink() for path in paths):
        raise ValueError('Unexpected symbolic link in iOS app')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
        for path in paths:
            if path.is_file():
                # zipfile.write preserves the executable permission needed by re-signing tools.
                archive.write(path, 'Payload/Cara.app/' + path.relative_to(app).as_posix())
    with zipfile.ZipFile(destination) as archive:
        assert archive.testzip() is None
        executable = archive.getinfo('Payload/Cara.app/' + name)
        if not (executable.external_attr >> 16) & stat.S_IXUSR:
            raise ValueError('IPA executable lost its execute permission')
    receipt = dict(source_revision=report.get('source_revision'), unsigned=True, requires_resigning=True, renderer=report.get('renderer'),
                   bundle_id=info['CFBundleIdentifier'], executable_sha256=hashlib.sha256((app / name).read_bytes()).hexdigest(),
                   ipa_sha256=hashlib.sha256(destination.read_bytes()).hexdigest())
    destination.with_suffix('.ipa.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--audit', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=Path(__file__).resolve().parents[1],text=True).strip()
    receipt=package(args.app, args.audit, args.output, expected_revision=revision)
    receipt['source_url']='https://github.com/dmikey/cara'
    args.output.with_suffix('.ipa.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
