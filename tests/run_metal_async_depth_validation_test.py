#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile changed production objects separately; never rebuild the application."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
from run_metal_texture_copy_batch_test import ninja_block


def main():
    root=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',type=Path,default=root/'work/build-native-metal-mac')
    p.add_argument('--compile-only',action='store_true')
    args=p.parse_args();build=args.build_dir.resolve()
    ninja=(build/'build.ninja').read_text()
    block=ninja_block(ninja,'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_native_draw_lane.mm.o')
    link=ninja_block(ninja,'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    flags=shlex.split(block['DEFINES'])+shlex.split(block['FLAGS'])+shlex.split(block['INCLUDES'])
    env=dict(os.environ);env.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
    compiler=subprocess.check_output(['xcrun','--find','clang++'],env=env,text=True).strip()
    sdk=subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],env=env,text=True).strip()
    output=root/'work/portable-metal-checks/async-depth';output.mkdir(parents=True,exist_ok=True)
    objects=[]
    for name in ('pica_metal_draw_group','pica_metal_submission_queue','pica_metal_native_draw_lane'):
        obj=output/(name+'.o');objects.append(str(obj))
        subprocess.run([compiler,*flags,'-c',str(root/'emulator/src/video_core/renderer_metal'/(name+'.mm')),'-o',str(obj)],cwd=build,env=env,check=True)
    test=output/'test.o'
    subprocess.run([compiler,*flags,'-c',str(root/'tests/metal_async_depth_validation_test.mm'),'-o',str(test)],cwd=build,env=env,check=True)
    binary=output/'async-depth-test'
    subprocess.run([compiler,'-isysroot',sdk,'-mmacosx-version-min=13.0','-Wl,-dead_strip',
                    str(test),*objects,*shlex.split(link['LINK_LIBRARIES']),'-o',str(binary)],
                   cwd=build,env=env,check=True)
    if args.compile_only:print('Built',binary);return
    results=[]
    for flag, pending in (('0','0'),('1','0'),('1','1')):
        child={k:v for k,v in env.items() if not k.startswith('CARA_')}
        child.update(CARA_METAL_ASYNC_CHECKED_DEPTH_IMPORT=flag,
                     CARA_METAL_QUEUE_PENDING_DEPTH_EXPORT=pending,MTL_DEBUG_LAYER='1')
        result=subprocess.run([str(binary),str(build/'src/video_core/cara-pica.metallib')],cwd=root,env=child,text=True,capture_output=True)
        results.append(dict(async_depth=flag,pending_depth=pending,exit_code=result.returncode,
                            stdout=result.stdout,stderr=result.stderr))
        (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(result.stdout,end='');print(result.stderr,end='');result.check_returncode()


if __name__=='__main__':main()
