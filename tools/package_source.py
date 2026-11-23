#!/usr/bin/env python3
"""Export a clean, tracked source tree with bundled dependencies and build scripts."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def package(output, revision):
    actual = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if revision != actual:
        raise ValueError('Source revision differs from checkout')
    if subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip():
        raise ValueError('Source archive requires a clean checkout')
    if output.exists():
        raise ValueError('Source output already exists')
    output.parent.mkdir(parents=True, exist_ok=True)
    prefix = 'Cara-source-' + revision + '/'
    subprocess.run(['git', 'archive', '--format=tar.gz', '--prefix=' + prefix,
                    '--output=' + str(output.resolve()), revision], cwd=ROOT, check=True)
    with tarfile.open(output) as archive:
        names = set(archive.getnames())
        for required in ('LICENSE', 'NOTICE', 'tools/build.py', 'emulator/VENDORED_SOURCE.json',
                         'emulator/src/ios/main.mm', 'emulator/externals/soundtouch/COPYING.txt'):
            if prefix + required not in names:
                raise ValueError('Source archive missing ' + required)
    receipt = dict(source_revision=revision, sha256=hashlib.sha256(output.read_bytes()).hexdigest())
    output.with_suffix(output.suffix + '.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--revision', required=True)
    args = parser.parse_args()
    print(json.dumps(package(args.output, args.revision)))
