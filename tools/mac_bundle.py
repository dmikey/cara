#!/usr/bin/env python3
"""Relocate the complete non-system Mach-O dependency closure into an app."""
import hashlib
import re
import shutil
import subprocess
from pathlib import Path


def output(*args):
    return subprocess.check_output(list(map(str, args)), text=True)


def system(name):
    return name.startswith(('/System/Library/', '/usr/lib/'))


def dependencies(binary):
    lines = output('/usr/bin/otool', '-L', binary).splitlines()[1:]
    return [line.strip().split(' (compatibility')[0] for line in lines if line.strip()]


def rpaths(binary):
    return re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset',
                      output('/usr/bin/otool', '-l', binary))


def minimum_os(binary):
    commands = output('/usr/bin/otool', '-l', binary)
    versions = re.findall(r'\bminos (\d+(?:\.\d+)*)', commands)
    if not versions:
        versions = re.findall(r'cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version (\d+(?:\.\d+)*)', commands)
    if not versions:
        raise RuntimeError('Missing macOS deployment version: ' + str(binary))
    return max(versions, key=version_key)


def version_key(value):
    parts = tuple(map(int, value.split('.')))
    return parts + (0,) * (3 - len(parts))


def resolve(name, owner, executable, inherited):
    candidates = []
    def expand(value):
        return value.replace('@loader_path', str(owner.parent)).replace('@executable_path', str(executable.parent))
    if name.startswith('@rpath/'):
        candidates = [Path(expand(base)) / name[len('@rpath/'):] for base in rpaths(owner) + inherited]
    else:
        candidates = [Path(expand(name))]
    for candidate in candidates:
        if candidate.is_absolute() and candidate.is_file():
            return candidate.resolve()
    raise RuntimeError('Unresolved non-system dependency: ' + name + ' from ' + str(owner))


def bundle(core, frameworks, sdl3=None):
    frameworks.mkdir()
    originals = {core: core}
    mapping = {}
    inherited = rpaths(core)
    pending = [core]
    # sdl2-compat loads this through dlopen, so otool alone cannot discover it.
    dynamic_sdl3_required = False
    while pending:
        source = pending.pop()
        for name in dependencies(source):
            if system(name):
                continue
            dependency = resolve(name, source, core, inherited)
            if dependency == source:
                continue  # a dylib's own LC_ID_DYLIB
            if dependency not in mapping:
                destination = frameworks / dependency.name
                if destination in mapping.values():
                    raise RuntimeError('Dependency filename collision: ' + str(destination))
                shutil.copy2(dependency, destination)
                mapping[dependency] = destination
                pending.append(dependency)
                if b'@loader_path/libSDL3.dylib' in dependency.read_bytes():
                    dynamic_sdl3_required = True
        if dynamic_sdl3_required and not any(p.name == 'libSDL3.dylib' for p in mapping.values()):
            if not sdl3 or not sdl3.is_file():
                raise RuntimeError('sdl2-compat requires --sdl3-library for its runtime-loaded SDL3 dependency')
            dependency = sdl3.resolve()
            destination = frameworks / 'libSDL3.dylib'
            if dependency not in mapping:
                shutil.copy2(dependency, destination)
                mapping[dependency] = destination
                pending.append(dependency)
    originals.update({destination: source for source, destination in mapping.items()})
    for destination, source in originals.items():
        changes = []
        if destination != core:
            changes += ['-id', '@rpath/' + destination.name]
        for name in dependencies(source):
            if system(name):
                continue
            dependency = resolve(name, source, core, inherited)
            if dependency == source:
                continue
            prefix = '@loader_path/../Frameworks/' if destination == core else '@loader_path/'
            changes += ['-change', name, prefix + mapping[dependency].name]
        for path in rpaths(destination):
            changes += ['-delete_rpath', path]
        if destination == core:
            changes += ['-add_rpath', '@executable_path/../Frameworks']
        subprocess.run(['/usr/bin/install_name_tool', *changes, str(destination)], check=True)
    return mapping


def audit(app):
    contents = app / 'Contents'
    core = contents / 'MacOS/cara-core'
    binaries = [contents / 'MacOS/Cara', core, *sorted((contents / 'Frameworks').glob('*.dylib'))]
    report = []
    for binary in binaries:
        architectures = output('/usr/bin/lipo', '-archs', binary).strip().split()
        if 'arm64' not in architectures:
            raise RuntimeError('Missing arm64 slice: ' + str(binary))
        for path in rpaths(binary):
            if not path.startswith(('@loader_path', '@executable_path')):
                raise RuntimeError('External runtime search path: ' + path)
        for name in dependencies(binary):
            if system(name):
                continue
            if name.startswith('@rpath/') and name == '@rpath/' + binary.name and binary.parent.name == 'Frameworks':
                continue
            target = resolve(name, binary, core, rpaths(core))
            if not target.is_relative_to(contents.resolve()):
                raise RuntimeError('Dependency escapes app: ' + name)
        report.append(dict(path=str(binary.relative_to(app)), architectures=architectures, minimum_os=minimum_os(binary),
                           sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
    return report
