#!/usr/bin/env python3
"""CPU-only actual ShaderSetup/source-cache regressions, with no app rebuild or GPU.

Standalone overrides are necessary when current engine archives predate the
ShaderSetup layout change. No ROM, shader compiler, Metal device or download.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
from run_metal_texture_copy_batch_test import ninja_block


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=root/'work/build-native-metal-mac')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    ninja = (build/'build.ninja').read_text()
    link = ninja_block(ninja, 'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    env = dict(os.environ)
    env.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    compiler = subprocess.check_output(['xcrun', '--find', 'clang++'], env=env, text=True).strip()
    output = root/'work/portable-metal-checks/shader-identity'
    output.mkdir(parents=True, exist_ok=True)
    def compile_file(source, name, flags_from):
        block = ninja_block(ninja, 'src/video_core/CMakeFiles/video_core.dir/'+flags_from+'.o')
        flags = shlex.split(block['DEFINES']) + shlex.split(block['FLAGS'])
        flags = [flag for flag in flags if not flag.startswith('-fprofile')]
        for item in shlex.split(block['INCLUDES']):
            flags += ['-isystem', item[2:]] if item.startswith('-I') and '/externals/' in item else [item]
        flags += ['-Wno-deprecated-declarations', '-Wno-deprecated-builtins', '-Wno-return-type']
        target = output/(name+'.o')
        subprocess.run([compiler, *flags, '-c', str(root/source), '-o', str(target)], cwd=build, env=env, check=True)
        return target
    shader = compile_file('emulator/src/video_core/shader/shader.cpp', 'shader', 'shader/shader.cpp')
    generator = compile_file('emulator/src/video_core/renderer_opengl/gl_shader_gen.cpp', 'generator', 'renderer_opengl/gl_shader_gen.cpp')
    lifecycle = compile_file('tests/shader_identity_lifecycle_test.cpp', 'lifecycle', 'shader/shader.cpp')
    source_cache = compile_file('tests/metal_shader_identity_cache_test.mm', 'source-cache', 'renderer_metal/pica_metal_native_bindings.mm')
    binaries = []
    for name, objects in [('lifecycle-test', [lifecycle, shader]), ('source-cache-test', [source_cache, generator])]:
        binary = output/name
        subprocess.run([compiler, '-mmacosx-version-min=13.0', '-Wl,-dead_strip', *map(str, objects),
                        *shlex.split(link['LINK_LIBRARIES']), '-o', str(binary)], cwd=build, env=env, check=True)
        binaries.append(binary)
    results = []
    for binary, reuse, validate in [(binaries[0], '0', '0'), (binaries[1], '0', '0'),
                                    (binaries[1], '1', '0'), (binaries[1], '1', '1')]:
        child = {key: value for key, value in env.items() if not key.startswith('CARA_')}
        child.update(CARA_METAL_REUSE_SHADER_IDENTITY=reuse, CARA_METAL_VALIDATE_SHADER_IDENTITY=validate)
        result = subprocess.run([str(binary)], env=child, text=True, capture_output=True)
        results.append(dict(test=binary.name, reuse=reuse, validate=validate,
                            exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr))
        (output/'results.json').write_text(json.dumps(results, indent=2)+'\n')
        print(result.stdout, end='')
        print(result.stderr, end='')
        if result.returncode:
            raise RuntimeError('Shader identity regression failed; see results.json')


if __name__ == '__main__':
    main()
