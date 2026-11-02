#!/usr/bin/env python3
"""Fetch the pinned public source, apply Cara's patch, and add port files."""
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'work' / 'citra-mmj'
DEPENDENCIES = [
    'externals/boost', 'externals/soundtouch', 'externals/cryptopp/cryptopp',
    'externals/fmt', 'externals/nihstro', 'externals/teakra', 'externals/enet',
    'externals/inih/inih', 'externals/libyuv', 'externals/catch', 'externals/xbyak',
]


def run(*args, cwd=SOURCE):
    subprocess.run(args, cwd=cwd, check=True)


def prepare():
    lock = json.loads((ROOT / 'upstream.lock.json').read_text())
    patch = ROOT / 'patches' / 'mmj-no-jit.patch'
    overlays = sorted(p for p in (ROOT / 'port').rglob('*') if p.is_file())
    digest = hashlib.sha256((ROOT / 'upstream.lock.json').read_bytes() + patch.read_bytes())
    for path in overlays:
        digest.update(str(path.relative_to(ROOT / 'port')).encode())
        digest.update(path.read_bytes())
    stamp = digest.hexdigest()
    marker = SOURCE / '.git' / 'cara-source-stamp'
    if marker.exists():
        if marker.read_text().strip() != stamp:
            raise SystemExit('Port inputs changed. Preserve any work in work/citra-mmj, then move that folder aside and rerun preparation.')
    else:
        if SOURCE.exists() and any(SOURCE.iterdir()):
            raise SystemExit('Refusing to overwrite an existing unmarked work/citra-mmj tree.')
        SOURCE.mkdir(parents=True, exist_ok=True)
        run('git', 'init')
        run('git', 'remote', 'add', 'origin', lock['repository'])
        run('git', 'fetch', '--depth', '1', 'origin', lock['revision'])
        run('git', 'checkout', '--detach', 'FETCH_HEAD')
        run('git', 'apply', '--check', str(patch))
        run('git', 'apply', str(patch))
        for path in overlays:
            target = SOURCE / path.relative_to(ROOT / 'port')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
        marker.write_text(stamp + '\n')
    run('git', 'submodule', 'sync')
    # Dynarmic is excluded; LibreSSL is unused with ENABLE_WEB_SERVICE=OFF.
    run('git', 'submodule', 'update', '--init', '--depth', '1', *DEPENDENCIES)
    for path in DEPENDENCIES:
        expected = subprocess.check_output(['git', 'ls-tree', 'HEAD', path], cwd=SOURCE, text=True).split()[2]
        actual = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=SOURCE / path, text=True).strip()
        if actual != expected:
            raise SystemExit('Dependency revision mismatch: ' + path)
    print('Prepared pinned source:', SOURCE)
    return SOURCE


if __name__ == '__main__':
    prepare()
