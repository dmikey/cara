#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise terminal-success writer memoization using a freshly rebuilt engine archive."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
from run_metal_texture_copy_batch_test import ninja_block


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,default=root/'work/build-native-metal-mac')
    parser.add_argument('--compile-only',action='store_true')
    parser.add_argument('--run-only',action='store_true')
    args=parser.parse_args();build=args.build_dir.resolve()
    archive=build/'src/video_core/libvideo_core.a'
    changed=[root/'emulator/src/video_core/renderer_metal'/name for name in
             ('pica_metal_submission_queue.h','pica_metal_submission_queue.mm','pica_metal_surface_writer.h','pica_metal_draw_group.h','pica_metal_draw_group.mm')]
    if not archive.is_file() or archive.stat().st_mtime < max(path.stat().st_mtime for path in changed):
        parser.error('Rebuild the complete matching engine first; no old-ABI overlays are supported')
    output=root/'work/portable-metal-checks/completed-writers';output.mkdir(parents=True,exist_ok=True)
    binary=output/'completed-writers-test'
    env=dict(os.environ);env.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
    if not args.run_only:
        graph=(build/'build.ninja').read_text()
        fields=ninja_block(graph,'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_native_draw_lane.mm.o')
        link=ninja_block(graph,'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
        flags=shlex.split(fields['DEFINES'])+shlex.split(fields['FLAGS'])+shlex.split(fields['INCLUDES'])
        compiler=re.search(r'^CMAKE_CXX_COMPILER:FILEPATH=(.*)$',(build/'CMakeCache.txt').read_text(),re.M)[1]
        obj=output/'test.o'
        compile_command=[compiler,*flags,'-c',str(root/'tests/metal_completed_writers_test.mm'),'-o',str(obj)]
        link_command=[compiler,*shlex.split(link.get('FLAGS','')),*shlex.split(link.get('LINK_FLAGS','')),
                      str(obj),*shlex.split(link['LINK_LIBRARIES']),'-o',str(binary)]
        (output/'commands.json').write_text(json.dumps([compile_command,link_command],indent=2)+'\n')
        subprocess.run(compile_command,cwd=build,env=env,check=True)
        subprocess.run(link_command,cwd=build,env=env,check=True)
    if args.compile_only:
        print('Built',binary);return
    results=[]
    for option,pred in (('0','0'),('1','0'),('0','1'),('1','1')):
        child={key:value for key,value in env.items() if not key.startswith('CARA_')}
        child.update(CARA_METAL_FRAME_COST='1',MTL_DEBUG_LAYER='1',
                     CARA_METAL_CACHE_COMPLETED_PREDECESSORS=pred,CARA_METAL_CACHE_COMPLETED_WRITERS=option)
        result=subprocess.run([str(binary)],cwd=root,env=child,text=True,capture_output=True)
        passed=result.returncode==0 and f'PASS completed_writer={option} predecessor={pred}' in result.stdout
        results.append(dict(option=option,predecessors=pred,passed=passed,exit_code=result.returncode,
                            stdout=result.stdout,stderr=result.stderr))
        (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(result.stdout,end='');print(result.stderr,end='')
        if not passed:raise RuntimeError(f'Completed writer validation failed: {option!r}')



if __name__=='__main__':main()
