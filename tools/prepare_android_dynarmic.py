#!/usr/bin/env python3
"""Fetch only the optional Android JIT dependency; iOS stays source-complete and jitless."""
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'emulator/externals/dynarmic'
REVISION = 'b3a92ab54dadd26a0c2a87d2677b80249d2e1a5a'
URL = 'https://github.com/azahar-emu/dynarmic.git'
def main():
    if not DEST.exists():
        DEST.mkdir(parents=True)
        subprocess.run(['git', 'init', str(DEST)], check=True)
        subprocess.run(['git', '-C', str(DEST), 'fetch', '--depth', '1', URL, REVISION], check=True)
        subprocess.run(['git', '-C', str(DEST), 'checkout', '--detach', 'FETCH_HEAD'], check=True)
    actual = subprocess.check_output(['git', '-C', str(DEST), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != REVISION:
        raise SystemExit('Existing Dynarmic checkout differs; refusing to overwrite it')
    patch = ROOT / 'tools/android-dynarmic-fmt6.patch'
    command = ['git', '-C', str(DEST), 'apply']
    if subprocess.run(command + ['--check', str(patch)], capture_output=True).returncode == 0:
        subprocess.run(command + [str(patch)], check=True)
    elif subprocess.run(command + ['--reverse', '--check', str(patch)], capture_output=True).returncode != 0:
        raise SystemExit('Dynarmic fmt compatibility patch does not match this source')
    print('Android Dynarmic pinned:', actual)
if __name__ == '__main__':
    main()
