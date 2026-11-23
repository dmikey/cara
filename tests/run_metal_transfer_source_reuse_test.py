#!/usr/bin/env python3
"""Authored source-borrow/export GPU regression against existing engine archives.

No production source mutation, ROM, shader dump, CMake rebuild or download.
Run only after coordinating a GPU slot outside performance measurements.
"""
import argparse
import json
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
    block=ninja_block(ninja,'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_native_draw_lane.mm.o')
    link=ninja_block(ninja,'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    flags=shlex.split(block['DEFINES'])+shlex.split(block['FLAGS'])+shlex.split(block['INCLUDES'])
    env=dict(os.environ);env.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
    compiler=subprocess.check_output(['xcrun','--find','clang++'],env=env,text=True).strip()
    output=root/'work/portable-metal-checks/transfer-source-reuse';output.mkdir(parents=True,exist_ok=True)
    subprocess.run([compiler,*flags,'-c',str(root/'tests/metal_transfer_source_reuse_test.mm'),'-o',str(output/'test.o')],
                   cwd=build,env=env,check=True)
    binary=output/'transfer-source-reuse-test'
    subprocess.run([compiler,'-mmacosx-version-min=13.0','-Wl,-dead_strip',str(output/'test.o'),
                    *shlex.split(link['LINK_LIBRARIES']),'-o',str(binary)],cwd=build,env=env,check=True)
    if args.compile_only:print('Built',binary);return
    results=[]
    for direct,striped in (('0','0'),('1','0'),('0','1'),('1','1')):
        child={key:value for key,value in env.items() if not key.startswith('CARA_')}
        child.update(CARA_METAL_DIRECT_EXPORT_CONSUMPTION=direct,
                     CARA_METAL_BORROW_STRIPED_SOURCE=striped,MTL_DEBUG_LAYER='1')
        result=subprocess.run([str(binary),str(build/'src/video_core/cara-pica.metallib')],
                              cwd=root,env=child,text=True,capture_output=True)
        receipt={'direct':direct,'striped':striped,'exit_code':result.returncode,
                 'stdout':result.stdout,'stderr':result.stderr}
        results.append(receipt)
        (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(result.stdout,end='');print(result.stderr,end='')
        if result.returncode:raise RuntimeError('Source-consumption GPU regression failed; retained results.json')


if __name__=='__main__':main()
