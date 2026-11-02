#!/usr/bin/env python3
"""Validate bundled source. This step never fetches upstream repositories."""
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'emulator'


def prepare():
    provenance = json.loads((SOURCE / 'VENDORED_SOURCE.json').read_text())
    required = ['CMakeLists.txt', 'src/core/core.cpp', 'src/ios/main.mm',
                'src/video_core/renderer_software.cpp', 'license.txt']
    for name in required:
        if not (SOURCE / name).is_file():
            raise SystemExit('Missing bundled source: ' + name)
    for dep in provenance['dependencies']:
        path = SOURCE / dep['path']
        if not path.is_dir() or not any(path.iterdir()):
            raise SystemExit('Missing bundled dependency: ' + dep['path'])
    for name in provenance['excluded_non_source']:
        if (SOURCE / name).exists():
            raise SystemExit('Excluded non-source assets are present: ' + name)
    # Reject gitlinks: an ordinary checkout must contain the actual source files.
    entries = subprocess.check_output(['git', 'ls-files', '--stage'], cwd=ROOT, text=True)
    gitlinks = [line for line in entries.splitlines() if line.startswith('160000 ')]
    if gitlinks:
        raise SystemExit('External source submodules are not allowed: ' + repr(gitlinks))
    print('Bundled emulator and', len(provenance['dependencies']), 'dependency source trees verified; no network fetch.')
    return SOURCE


if __name__ == '__main__':
    prepare()
