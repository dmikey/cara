#!/usr/bin/env python3
"""Production display crop fusion and buffer-only color GPU regression.

No production source mutation, ROM, shader dump, CMake rebuild or download.
The complete legacy/optimized matrix uses API validation. Shader validation
also checks the optimized-only subset against identical control bytes: the
legacy buffer exporter currently returns black RGB under shader validation,
independently reproduced by direct texture/authority boundary probes.
CARA_METAL_TEST_BOUNDARY_PROBES=1 enables authored source boundary readbacks.
Run only after coordinating a GPU slot outside performance measurements.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import plistlib
import subprocess
from run_metal_texture_copy_batch_test import ninja_block


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,default=root/'work/build-native-metal-mac')
    parser.add_argument('--compile-only',action='store_true')
    parser.add_argument('--lane-overlay',action='store_true',help='Compile changed lane directly before unchanged archives; no app rebuild')
    parser.add_argument('--reuse-lane-overlay',action='store_true')
    args=parser.parse_args();build=args.build_dir.resolve()
    ninja=(build/'build.ninja').read_text()
    block=ninja_block(ninja,'src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_native_draw_lane.mm.o')
    link=ninja_block(ninja,'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom')
    flags=shlex.split(block['DEFINES'])+shlex.split(block['FLAGS'])+shlex.split(block['INCLUDES'])
    env=dict(os.environ);env.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
    compiler=subprocess.check_output(['xcrun','--find','clang++'],env=env,text=True).strip()
    output=root/'work/portable-metal-checks/display-fusion';output.mkdir(parents=True,exist_ok=True)
    lane_objects=[]
    if args.lane_overlay:
        lane_object=output/'lane.o'
        subprocess.run([compiler,*flags,'-c',str(root/'emulator/src/video_core/renderer_metal/pica_metal_native_draw_lane.mm'),'-o',str(lane_object)],cwd=build,env=env,check=True)
        lane_objects=[str(lane_object)]
    if args.reuse_lane_overlay:lane_objects=[str(output/'lane.o')]
    subprocess.run([compiler,*flags,'-c',str(root/'tests/metal_display_fusion_test.mm'),'-o',str(output/'test.o')],
                   cwd=build,env=env,check=True)
    bundle=output/'display-fusion-test.app'; executable=bundle/'Contents/MacOS'; executable.mkdir(parents=True,exist_ok=True)
    binary=executable/'display-fusion-test'
    subprocess.run([compiler,'-mmacosx-version-min=13.0','-Wl,-dead_strip',str(output/'test.o'),*lane_objects,
                    *shlex.split(link['LINK_LIBRARIES']),'-o',str(binary)],cwd=build,env=env,check=True)
    if args.compile_only:print('Built',binary);return
    resources=bundle/'Contents/Resources'; resources.mkdir(parents=True,exist_ok=True)
    for name in ('cara-pica.metallib','cara-scaled-crop-rgb8.metallib'):
        shutil.copy2(build/'src/video_core'/name,resources/name)
    (bundle/'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable':binary.name,'CFBundleIdentifier':'org.cara.test.display-fusion','CFBundlePackageType':'APPL'}))
    results=[]; baseline=None
    for crop,linear,shader_validation,fast_only in (('0','0','0','0'),('1','0','0','0'),('0','1','0','0'),('1','1','0','0'),('1','1','1','1')):
        child={key:value for key,value in env.items() if not key.startswith('CARA_')}
        child.update(CARA_METAL_FUSE_TEXTURE_CROP=crop,
                     CARA_METAL_LINEAR_COLOR_BYTES_ONLY=linear,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION=shader_validation,CARA_METAL_TEST_FUSED_ONLY=fast_only)
        result=subprocess.run([str(binary),str(build/'src/video_core/cara-pica.metallib')],
                              cwd=root,env=child,text=True,capture_output=True)
        receipt={'crop':crop,'linear':linear,'shader_validation':shader_validation,'fast_only':fast_only,'exit_code':result.returncode,
                 'stdout':result.stdout,'stderr':result.stderr}
        results.append(receipt)
        (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print('crop='+crop+' linear='+linear+' shader_validation='+shader_validation+' fast_only='+fast_only+' '+(result.stdout.splitlines()[-1] if result.stdout.splitlines() else 'NO STDOUT'));print(result.stderr,end='')
        if result.returncode:raise RuntimeError('Display fusion GPU regression failed; retained results.json')
        cases=[line for line in result.stdout.splitlines() if line.startswith('CASE ')]
        if baseline is None:baseline=cases
        elif fast_only=='0' and cases!=baseline:raise RuntimeError('Optimized display bytes differ from production control')
        elif fast_only=='1' and any(line not in baseline for line in cases):raise RuntimeError('Shader-validated fast-path bytes differ from production control')
    print('PASS all flag combinations match exact production control bytes')


if __name__=='__main__':main()
