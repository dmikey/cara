#!/usr/bin/env python3
"""Build/run OFF/ON exact raster-state GPU regression using existing Metal archives."""
import argparse
import hashlib
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
    block=ninja_block(ninja,'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_draw.mm.o')
    link=ninja_block(ninja,'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    flags=shlex.split(block['DEFINES'])+shlex.split(block['FLAGS'])+shlex.split(block['INCLUDES'])
    env=dict(os.environ);env.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
    compiler=subprocess.check_output(['xcrun','--find','clang++'],env=env,text=True).strip()
    output=root/'work/portable-metal-checks/raster-state';output.mkdir(parents=True,exist_ok=True)
    for source,obj in ((root/'emulator/src/video_core/renderer_metal/pica_metal_draw.mm','draw.o'),
                       (root/'tests/metal_raster_state_reuse_test.mm','test.o')):
        subprocess.run([compiler,*flags,'-c',str(source),'-o',str(output/obj)],cwd=build,env=env,check=True)
    binary=output/'raster-state-test'
    subprocess.run([compiler,'-mmacosx-version-min=13.0','-Wl,-dead_strip',str(output/'test.o'),str(output/'draw.o'),
                    *shlex.split(link['LINK_LIBRARIES']),'-o',str(binary)],cwd=build,env=env,check=True)
    if args.compile_only:print('Built',binary);return
    for value in ('0','1'):
        child={key:val for key,val in env.items() if not key.startswith('CARA_')}
        child.update(CARA_METAL_REUSE_RASTER_STATE=value,MTL_DEBUG_LAYER='1')
        subprocess.run([str(binary),str(output/('pixels-'+value+'.rgba'))],cwd=root,env=child,check=True)
    control=(output/'pixels-0.rgba').read_bytes();candidate=(output/'pixels-1.rgba').read_bytes()
    if control!=candidate:raise RuntimeError('Raster-state OFF/ON GPU pixels differ')
    print('Exact OFF/ON pixels SHA256',hashlib.sha256(control).hexdigest())


if __name__=='__main__':main()
