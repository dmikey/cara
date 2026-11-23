#!/usr/bin/env python3
"""Bundle offline notices and exact source identity for the iOS About screen."""
import argparse
import fnmatch
import json
import re
import hashlib
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def bundle(app, extra_licenses=()):
    destination = app / 'Legal'
    destination.mkdir(parents=True, exist_ok=True)
    for name in ('LICENSE', 'NOTICE'):
        (destination / (name + '.txt')).write_bytes((ROOT / name).read_bytes())
    notices = []
    for base in (ROOT / 'emulator/externals', ROOT / 'emulator/misc/licenses', ROOT / 'emulator/src/ios/MenuSounds', ROOT / 'emulator/src/ios/ControlPresets'):
        for path in sorted(base.rglob('*')):
            if path.is_file() and any(fnmatch.fnmatch(path.name.lower(), pattern) for pattern in ('license*', 'copying*', 'copyright*', 'notice*')):
                notices.append(path.relative_to(ROOT).as_posix() + '\n\n' + path.read_text(errors='replace'))
    for base in extra_licenses:
        if not str(base) or not Path(base).is_dir():
            continue
        base=Path(base)
        for path in sorted(base.rglob('*')):
            if path.is_file() and any(fnmatch.fnmatch(path.name.lower(), pattern) for pattern in ('license*', 'copying*', 'copyright*', 'notice*')):
                notices.append('external/' + base.name + '/' + path.relative_to(base).as_posix() + '\n\n' + path.read_text(errors='replace'))
    # These dependencies keep their license text in source headers, rather than
    # a conventionally named LICENSE file. Preserve the actual upstream blocks.
    for name, mode in (
        ('emulator/externals/glad/include/KHR/khrplatform.h', 'block'),
        ('emulator/externals/json/json.hpp', 'block'),
        ('emulator/externals/microprofile/microprofile.h', 'lines'),
        ('emulator/externals/minilzo/minilzo.c', 'block'),
        ('emulator/externals/soundtouch/include/SoundTouch.h', 'preamble'),
    ):
        text = (ROOT / name).read_text()
        if mode == 'block':
            match = re.search(r'/\*.*?\*/', text, re.S)
            if match is None:
                raise RuntimeError('Missing embedded license: ' + name)
            text = match.group()
        elif mode == 'preamble':
            text = text.split('#ifndef', 1)[0]
        else:
            text = text.split('// ***********************************************************************')[0]
        notices.append(name + '\n\n' + text)
    if not notices:
        raise RuntimeError('No dependency notices found')
    (destination / 'THIRD_PARTY.txt').write_text('\n\n' + ('\n\n' + '=' * 72 + '\n\n').join(notices))
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=normal'], cwd=ROOT, text=True).strip())
    metadata = dict(revision=revision, modified=dirty,
                    source_url='https://github.com/dmikey/cara/tree/' + revision,
                    archive_url='https://github.com/dmikey/cara/archive/' + revision + '.tar.gz')
    metadata['legal_sha256'] = {name: hashlib.sha256((destination / name).read_bytes()).hexdigest() for name in ('LICENSE.txt', 'NOTICE.txt', 'THIRD_PARTY.txt')}
    (destination / 'source.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--extra-licenses', nargs='*', default=[])
    args=parser.parse_args()
    bundle(args.app, [Path(p) for p in args.extra_licenses if p])
