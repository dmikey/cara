#!/usr/bin/env python3
"""Package the Linux SDL/OpenGL host with source identity, licenses and linkage evidence."""
import argparse
import fnmatch
import hashlib
import json
import platform
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != 'Linux':
        parser.error('Packaging requires a native Linux build')
    build = args.build_dir.resolve()
    binary = build / 'bin/cara'
    cache = (build / 'CMakeCache.txt').read_text()
    for setting in ('CARA_LINUX_SDL_HOST:BOOL=ON', 'CARA_ENABLE_JIT:BOOL=OFF',
                    'CARA_SOFTWARE_PRESENTATION:BOOL=OFF', 'ENABLE_SDL2:BOOL=ON'):
        if setting not in cache:
            raise RuntimeError('Build configuration mismatch: ' + setting)
    symbols = subprocess.check_output(['nm', '-C', str(binary)], text=True)
    if any(name in symbols for name in ('ARM_Dynarmic', 'JitX64Engine')):
        raise RuntimeError('Unexpected JIT backend in Linux FastInterp build')
    linkage = subprocess.check_output(['ldd', str(binary)], text=True)
    if 'not found' in linkage:
        raise RuntimeError('Unresolved runtime dependency: ' + linkage)
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stage = out / 'Cara-Linux-x86_64-OpenGL'
    if stage.exists():
        raise RuntimeError('Refusing to overwrite package staging directory')
    stage.mkdir()
    shutil.copy2(binary, stage / 'cara')
    for name in ('LICENSE', 'NOTICE'):
        shutil.copy2(ROOT / name, stage / name)
    licenses = stage / 'licenses'
    for path in (ROOT / 'emulator').rglob('*'):
        if path.is_file() and any(fnmatch.fnmatch(path.name.lower(), p)
                                  for p in ('license*', 'copying*', 'copyright*')):
            target = licenses / path.relative_to(ROOT / 'emulator')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
    for package in ('libsdl2-2.0-0', 'libpng16-16t64'):
        path = Path('/usr/share/doc') / package / 'copyright'
        if path.is_file():
            licenses.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, licenses / (package + '-copyright'))
    report = dict(source_revision=revision, platform='linux', architecture=platform.machine(),
                  cpu_backend='fastinterp', cpu_jit=False, shader_jit=False,
                  renderer='desktop-opengl', minimum_opengl='4.3',
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  linkage=linkage, source_url='https://github.com/dmikey/cara/tree/' + revision)
    (stage / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
    (stage / 'README.txt').write_text(
        'Cara Linux x86_64 OpenGL developer build\n\n'
        'Built on Ubuntu 24.04. Requires desktop OpenGL 4.3 or newer.\n'
        'Ubuntu runtime dependencies:\n'
        '  sudo apt install libsdl2-2.0-0 libpng16-16t64 libgl1 libgl1-mesa-dri\n\n'
        'Run: ./cara /path/to/game.3ds\n'
        'Check graphics without a game: ./cara --self-test\n'
        'Use --fullscreen for fullscreen; Escape closes the window.\n'
        'A/B/X/Y: A/S/Z/X; L/R: Q/W; Start/Select: Enter/Backspace.\n'
        'Circle Pad: arrows; D-pad: T/G/F/H; C-stick: I/K/J/L.\n'
        'Click the lower screen for touch input. Focus loss releases held keys.\n'
        'Uses FastInterp without JIT, native-resolution OpenGL, 100% speed and SDL audio.\n'
        'This first Linux frontend is command-line based, without the Apple library UI.\n'
        'FFmpeg/AAC decoding is not included in this build.\n'
        'Saves use the normal Linux Cara user directory; CARA_USER_DIR overrides it.\n\n'
        'Corresponding source and build instructions: ' + report['source_url'] + '\n')
    archive = out / (stage.name + '.tar.gz')
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(stage, arcname=stage.name)
    shutil.rmtree(stage)
    (out / 'linux-build.json').write_text(json.dumps(report, indent=2) + '\n')
    (out / 'SHA256SUMS.txt').write_text(
        hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
    print(archive)


if __name__ == '__main__':
    main()
