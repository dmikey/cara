#!/usr/bin/env python3
"""Build/run authored abort-cleanup regression using an existing Metal build.

Compiles changed owner/encoder objects directly; no CMake rebuild or ROM is used.
Run outside timing campaigns. Pass --compile-only to defer GPU validation.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
from run_metal_texture_copy_batch_test import ninja_block


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,default=root/'work/build-native-metal-mac')
    parser.add_argument('--compile-only',action='store_true')
    args=parser.parse_args();build=args.build_dir.resolve()
    ninja=(build/'build.ninja').read_text()
    link=ninja_block(ninja,'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    env=dict(os.environ);env.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
    compiler=subprocess.check_output(['xcrun','--find','clang++'],env=env,text=True).strip()
    output=root/'work/portable-metal-checks/abort-cleanup';output.mkdir(parents=True,exist_ok=True)
    objects=[]
    sources=['pica_metal_draw.mm','pica_metal_draw_group.mm','pica_metal_submission_queue.mm',
             'pica_metal_native_draw_lane.mm']
    def flags_for(name):
        block=ninja_block(ninja,'src/video_core/CMakeFiles/video_core.dir/renderer_metal/'+name+'.o')
        return shlex.split(block['DEFINES'])+shlex.split(block['FLAGS'])+shlex.split(block['INCLUDES'])
    for name in sources:
        obj=output/(name+'.o');objects.append(str(obj))
        subprocess.run([compiler,*flags_for(name),'-c',str(root/'emulator/src/video_core/renderer_metal'/name),
                        '-o',str(obj)],cwd=build,env=env,check=True)
    test=output/'test.o'
    subprocess.run([compiler,*flags_for(sources[-1]),'-Wextra','-Wno-deprecated-declarations',
                    '-Wno-unused-parameter','-c',str(root/'tests/metal_abort_cleanup_test.mm'),'-o',str(test)],
                   cwd=build,env=env,check=True)
    binary=output/'abort-cleanup-test'
    subprocess.run([compiler,'-mmacosx-version-min=13.0','-Wl,-dead_strip',str(test),*objects,
                    *shlex.split(link['LINK_LIBRARIES']),'-o',str(binary)],cwd=build,env=env,check=True)
    if args.compile_only:
        print('Built',binary);return
    # Isolate this authored fixture from inherited experimental flags. Metal
    # validation must remain enabled so an unended encoder fails the process.
    child={key:value for key,value in env.items() if not key.startswith('CARA_')}
    child['MTL_DEBUG_LAYER']='1'
    subprocess.run([str(binary),str(build/'src/video_core/cara-pica.metallib')],
                   cwd=root,env=child,check=True)


if __name__=='__main__':main()
