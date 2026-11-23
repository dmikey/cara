#!/usr/bin/env python3
"""Check real compute/raster batches with and without exact deferred uniform reuse."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
from run_metal_generated_vertex_options_test import ninja_block


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=root/'work/build-native-metal-mac')
    parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    ninja = (build/'build.ninja').read_text()
    target = ninja_block(ninja, 'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_draw.mm.o')
    link = ninja_block(ninja, 'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    output = root/'work/portable-metal-checks/unique-transform-admission'
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    compiler = subprocess.check_output(['xcrun','--find','clang++'],env=env,text=True).strip()
    flags = shlex.split(target['DEFINES']) + shlex.split(target['FLAGS'])
    for item in shlex.split(target['INCLUDES']):
        flags += ['-isystem',item[2:]] if item.startswith('-I') and '/externals/' in item else [item]
    objects = []
    for source in ('tests/metal_unique_vertex_compute_test.mm',
                   'emulator/src/video_core/renderer_metal/pica_metal_draw.mm'):
        obj = output/(Path(source).stem+'.o')
        subprocess.run([compiler,*flags,'-c',str(root/source),'-o',str(obj)],cwd=build,env=env,check=True)
        objects.append(str(obj))
    binary = output/'unique-vertex-compute-test'
    subprocess.run([compiler,'-mmacosx-version-min=13.0','-Wl,-dead_strip',*objects,
                    *shlex.split(link['LINK_LIBRARIES']),'-o',str(binary)],cwd=build,env=env,check=True)
    if args.compile_only:
        print('Built',binary)
        return
    results = []
    for reuse in ('0','1'):
        for dedup in ('0','1'):
            for admission in ('0','1'):
                for fallbacks in ('0','1'):
                    child = {k:v for k,v in env.items() if not k.startswith('CARA_')}
                    child.update(CARA_METAL_REUSE_DEFERRED_UNIFORMS=reuse,
                                 CARA_METAL_DEDUP_VERTEX_TRANSFORMS=dedup,
                                 CARA_METAL_UNIQUE_TRANSFORM_ADMISSION=admission,
                                 CARA_METAL_DEFER_RASTER_FALLBACKS=fallbacks,
                                 CARA_METAL_DRAW_GROUP_64='1',
                                 MTL_DEBUG_LAYER='1', MTL_SHADER_VALIDATION='1')
                    result = subprocess.run([str(binary)],cwd=root,env=child,text=True,capture_output=True)
                    receipt = dict(reuse=reuse,dedup=dedup,admission=admission,fallbacks=fallbacks,exit_code=result.returncode,
                                   stdout=result.stdout,stderr=result.stderr)
                    results.append(receipt)
                    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
                    print(next((line for line in result.stdout.splitlines() if line.startswith('PASS:')),result.stdout.strip()),flush=True)
                    if result.returncode:
                        raise RuntimeError(result.stderr)
                    profile=json.loads(next(line.removeprefix('ADMISSION_PROFILE ') for line in result.stdout.splitlines()
                                            if line.startswith('ADMISSION_PROFILE ')))['metal_deferred_admission_totals']
                    assert (profile['raster_only_packets']>0)==(fallbacks=='1')
                    assert profile['reasons']['admitted']>0
                    assert profile['reasons']['transform_limit']>0
                    assert (profile['reasons']['legacy_corner_limit']>0)==(admission=='0')
                    assert (profile['reasons']['raster_corner_limit']>0)==(admission=='1')
                    assert profile['max_output_bytes']<=64*8192*128
                    if admission=='1' and dedup=='0':
                        assert profile['max_output_bytes']==64*8192*128


if __name__ == '__main__':
    main()
