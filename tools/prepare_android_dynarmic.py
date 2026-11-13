#!/usr/bin/env python3
"""Fetch only the optional Android JIT dependency; iOS stays source-complete and jitless."""
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'emulator/externals/dynarmic'
REVISION = '2d3c8c5724800534f68e801e3f18b13a4d400e22'
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
    print('Android Dynarmic pinned:', actual)
if __name__ == '__main__':
    main()
