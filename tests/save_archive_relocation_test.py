#!/usr/bin/env python3
"""Run the actual C++ serializers in fresh processes; no ROM or real user data."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def snapshot(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('output', type=Path, help='parent for newly created private fixture directory')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix='archive-relocation-', dir=args.output.resolve()))
    results = []
    for case in ('old-exists', 'old-missing'):
        directory = run / case
        a, b, external = [directory / name for name in ('A', 'B', 'external')]
        state = directory / 'authored-state.bin'
        a.mkdir(parents=True)
        external.mkdir()
        (external / 'sentinel.bin').write_bytes(b'OUTSIDE-USER-ROOT')
        external_before = snapshot(external)

        def invoke(mode, root):
            result = subprocess.run([str(args.executable.resolve()), mode, str(root), str(state),
                                     str(external)], capture_output=True, text=True)
            results.append(dict(case=case, mode=mode, returncode=result.returncode,
                                stdout=result.stdout, stderr=result.stderr))
            (run / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            print(result.stdout, end='')

        invoke('save', a)
        original = snapshot(a)
        shutil.copytree(a, b)
        old = a
        if case == 'old-missing':
            old = directory / 'A-removed'
            a.rename(old)
        invoke('load', b)
        assert snapshot(old) == original, 'original root sentinel files changed'
        assert snapshot(external) == external_before, 'external sentinel changed'
        assert (b / 'open-file.bin').read_bytes() == b'IO-REBASED!'
        changed = [p for p in b.rglob('*.bin') if p.name != 'open-file.bin']
        assert len(changed) == 6, f'expected six authored files, got {len(changed)}'
        assert all(p.read_bytes() == b'RELOCATE' for p in changed), 'new-root save write missing'
        if case == 'old-missing':
            assert not a.exists(), 'load recreated old root'
        results.append(dict(case=case, original_root_unchanged=True, external_unchanged=True,
                            new_root_files_written=6, restored_iofile_rebased=True))
    (run / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(f'PASS: old-existing and old-missing relocation; evidence {run}')


if __name__ == '__main__':
    main()
