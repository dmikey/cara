#!/usr/bin/env python3
"""Build/run learned cache failure regression using existing Metal archives."""
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
    block=ninja_block(ninja,'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_pipelines.mm.o')
    link=ninja_block(ninja,'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    flags=shlex.split(block['DEFINES'])+shlex.split(block['FLAGS'])+shlex.split(block['INCLUDES'])
    env=dict(os.environ);env.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
    compiler=subprocess.check_output(['xcrun','--find','clang++'],env=env,text=True).strip()
    output=root/'work/portable-metal-checks/pipeline-cache-failure';output.mkdir(parents=True,exist_ok=True)
    for source,obj in ((root/'emulator/src/video_core/renderer_metal/pica_metal_pipelines.mm','pipelines.o'),
                       (root/'tests/metal_pipeline_cache_failure_test.mm','test.o')):
        subprocess.run([compiler,*flags,'-c',str(source),'-o',str(output/obj)],cwd=build,env=env,check=True)
    binary=output/'pipeline-cache-failure-test'
    subprocess.run([compiler,'-mmacosx-version-min=13.0','-Wl,-dead_strip',str(output/'test.o'),str(output/'pipelines.o'),
                    *shlex.split(link['LINK_LIBRARIES']),'-o',str(binary)],cwd=build,env=env,check=True)
    if args.compile_only:print('Built',binary);return
    child={key:value for key,value in env.items() if not key.startswith('CARA_')}
    child['MTL_DEBUG_LAYER']='1'
    import tempfile
    with tempfile.TemporaryDirectory(prefix='cara-cache-failure-',dir=output) as directory:
        result=subprocess.run([str(binary),directory],cwd=root,env=child,text=True,capture_output=True)
    (output/'validation.log').write_text(result.stdout+result.stderr)
    print(result.stdout,end='');print(result.stderr,end='')
    result.check_returncode()


if __name__=='__main__':main()
