#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run a reproducible FastInterp/native-Metal baseline or instrumented capture."""
import argparse
import csv
import hashlib
import json
import os
import math
from pathlib import Path
import re
import shutil
import subprocess
import statistics
import stat
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]


def file_sha256(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):digest.update(chunk)
    return digest.hexdigest()


def validate_cpu_backend(build, binary_sha256, backend):
    if backend == 'fastinterp':return
    if backend != 'jit':raise ValueError('Unsupported CPU backend')
    try:producer=json.loads((build/'native-metal-audit.json').read_text())
    except (OSError,ValueError) as error:
        raise ValueError('Explicit JIT diagnostic requires a binary-matched native Metal JIT audit') from error
    if producer.get('binary_sha256')!=binary_sha256 or producer.get('cpu_jit_enabled') is not True:
        raise ValueError('Explicit JIT diagnostic requires a binary-matched native Metal JIT audit')


def _clone_input_file(source, destination):
    """Try Darwin's independent copy-on-write clone, never a hardlink."""
    if sys.platform!='darwin':return False
    import ctypes
    try:clonefile=ctypes.CDLL('/usr/lib/libSystem.B.dylib',use_errno=True).clonefile
    except (AttributeError,OSError):return False
    clonefile.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint32]
    clonefile.restype=ctypes.c_int
    # SDK sys/clonefile.h: do not follow the source link or copy ownership.
    if clonefile(os.fsencode(source),os.fsencode(destination),0x0001|0x0002):
        error=ctypes.get_errno()
        raise OSError(error,os.strerror(error),str(source))
    return True


def copy_input_file(source, destination):
    """Seed one fresh, independent file, preserving source metadata and bytes.

    APFS shares extents until either copy is written. Unsupported cloning falls
    back to ordinary copying. Failed attempts leave no partial final file.
    """
    source=Path(source);destination=Path(destination)
    before=source.lstat()
    if not stat.S_ISREG(before.st_mode):raise ValueError(f'Nonregular input file: {source}')
    if os.path.lexists(destination):raise FileExistsError(f'Input destination already exists: {destination}')
    descriptor,name=tempfile.mkstemp(prefix='.cara-input-copy-',dir=destination.parent)
    os.close(descriptor);temporary=Path(name);temporary.unlink()
    claimed=None
    try:
        try:cloned=_clone_input_file(source,temporary)
        except OSError:cloned=False
        if not cloned:
            # A failed clone may leave a partial file or link. Remove only the
            # owned temporary name before the ordinary non-following copy.
            temporary.unlink(missing_ok=True)
            shutil.copy2(source,temporary,follow_symlinks=False)
        after=source.lstat();copied=temporary.lstat()
        identity=lambda entry:(entry.st_dev,entry.st_ino,entry.st_size,entry.st_mtime_ns,entry.st_ctime_ns)
        if identity(before)!=identity(after) or not stat.S_ISREG(after.st_mode):
            raise ValueError(f'Input changed during preparation: {source}')
        if not stat.S_ISREG(copied.st_mode) or (copied.st_dev,copied.st_ino)==(before.st_dev,before.st_ino):
            raise ValueError(f'Input copy is not an independent regular file: {destination}')
        # Claim the final name exclusively so an existing file/link is never
        # overwritten. It remains empty only until the atomic publication.
        descriptor=os.open(destination,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        try:claimed=os.fstat(descriptor)
        finally:os.close(descriptor)
        os.replace(temporary,destination)
        claimed=None
        return cloned
    finally:
        temporary.unlink(missing_ok=True)
        if claimed is not None:
            try:current=destination.lstat()
            except FileNotFoundError:current=None
            if current is not None and (current.st_dev,current.st_ino)==(claimed.st_dev,claimed.st_ino):
                destination.unlink()


def check_scene_reference(output, reference):
    """Coarse cross-build scene guard; never replaces exact optimization comparison."""
    def read(folder):
        layout=json.loads((folder/'metal-benchmark-frame.json').read_text())
        raw=(folder/'metal-benchmark-frame.bgra').read_bytes()
        width,height,stride=layout['width'],layout['height'],layout['row_stride']
        if layout['format']!='BGRA8' or width<=0 or height<=0 or stride<width*4 or len(raw)!=height*stride:
            raise ValueError('Invalid scene reference framebuffer')
        pixels=b''.join(raw[y*stride:y*stride+width*4] for y in range(height))
        return layout,pixels
    layout,pixels=read(output);other,expected=read(reference)
    same_layout=all(layout.get(k)==other.get(k) for k in ('format','origin','width','height','top','bottom'))
    result={'reference':str(reference.resolve()),'reference_pixels_sha256':hashlib.sha256(expected).hexdigest(),
            'same_layout':same_layout,
            'scope':'Coarse scene identity only: at least99.5% of pixels within3 channel units and mean absolute channel difference<=0.5. Exact same-build comparisons remain mandatory.'}
    if not same_layout:
        return result|{'passed':False}
    distant=0;total_delta=0
    for offset in range(0,len(pixels),4):
        differences=[abs(pixels[offset+j]-expected[offset+j]) for j in range(4)]
        distant+=max(differences)>3;total_delta+=sum(differences)
    ratio=distant/(len(pixels)//4);mean=total_delta/len(pixels)
    return result|{'distant_pixel_fraction':ratio,'mean_absolute_channel_difference':mean,
                   'passed':ratio<=0.005 and mean<=0.5}


def seed_state_filesystem(seed, output):
    """Copy only guest filesystem roots; reject links before copying any entries."""
    entries=[]
    for name in ('sdmc','nand','sysdata'):
        source=seed/name
        if source.is_symlink():raise ValueError(f'Symlinked seed entry: {source}')
        if not source.exists():continue
        if not source.is_dir():raise ValueError(f'Seed root is not a directory: {source}')
        entries.append((source,True))
        for current,dirs,files in os.walk(source,followlinks=False):
            for child in sorted(dirs+files):
                path=Path(current)/child
                if path.is_symlink():raise ValueError(f'Symlinked seed entry: {path}')
                if not path.is_dir() and not path.is_file():raise ValueError(f'Nonregular seed entry: {path}')
                entries.append((path,path.is_dir()))
    records=[]
    for source,is_directory in sorted(entries,key=lambda item:str(item[0])):
        relative=source.relative_to(seed);destination=output/relative
        if is_directory:
            destination.mkdir(parents=True,exist_ok=True)
            records.append({'path':str(relative),'kind':'directory'})
        else:
            destination.parent.mkdir(parents=True,exist_ok=True)
            copy_input_file(source,destination)
            if destination.is_symlink():raise ValueError(f'Seed entry changed to a symlink: {source}')
            records.append({'path':str(relative),'kind':'file','bytes':destination.stat().st_size,
                            'sha256':file_sha256(destination)})
    raw=(json.dumps(records,sort_keys=True,indent=2)+'\n').encode()
    (output/'state-filesystem-manifest.json').write_bytes(raw)
    return hashlib.sha256(raw).hexdigest()


def read_refresh_intervals(path, warmup, duration_column='wall_ms'):
    if not path.is_file():return None
    values={};previous=-1
    with path.open(newline='') as stream:
        reader=csv.DictReader(stream)
        if reader.fieldnames!=['refresh',duration_column]:raise ValueError('Invalid refresh CSV columns')
        for row in reader:
            refresh=int(row['refresh']);milliseconds=float(row[duration_column])
            if refresh<=previous or not math.isfinite(milliseconds) or milliseconds<0:
                raise ValueError('Invalid refresh CSV interval')
            previous=refresh
            if refresh>warmup:values[refresh]=milliseconds
    return values


def summarize_refresh_tail(wall, cpu=None, queue=None):
    if not wall:
        return None
    window_count = 120
    refreshes = list(wall)
    windows = (refreshes[index:index + window_count]
               for index in range(len(refreshes) - window_count + 1)
               if refreshes[index + window_count - 1] - refreshes[index] == window_count - 1)
    worst = max(windows, key=lambda window: sum(wall[frame] for frame in window), default=None)
    tail_count = math.ceil(len(wall) / 10)
    tail = sorted(wall, key=wall.get, reverse=True)[:tail_count]

    def describe(group):
        row = {'samples': len(group),
               'mean_wall_ms': statistics.fmean(wall[frame] for frame in group)}
        if cpu is not None:
            row['thread_cpu_samples'] = sum(frame in cpu for frame in group)
            if row['thread_cpu_samples'] == len(group):
                row['mean_thread_cpu_ms'] = statistics.fmean(cpu[frame] for frame in group)
        if queue is not None:
            row['mean_oldest_queue_wait_ms'] = statistics.fmean(
                queue.get(frame, 0) for frame in group)
        return row

    result = {'slowest_decile': describe(tail), 'all_refreshes': describe(refreshes),
              'scope': 'Completed RunLoop refreshes. Queue waits are a subset of wall time; '
                       'thread CPU and queue timers may overlap.'}
    if worst:
        result['worst_rolling_120'] = {
            **describe(worst), 'first_refresh': worst[0], 'last_refresh': worst[-1],
        }
    return result


def summarize_refresh_intervals(path, warmup):
    values=read_refresh_intervals(path,warmup)
    if values is None:return None
    if not values:return {'samples':0,'scope':'RunLoop/VBlank boundaries after warmup; not physical scanout or unique game FPS.'}
    ordered=sorted(values.values())
    def percentile(p):return ordered[max(0,math.ceil(p*len(ordered))-1)]
    return {'samples':len(values),'mean_wall_ms':statistics.fmean(ordered),
            'median_wall_ms':statistics.median(ordered),'p95_wall_ms':percentile(.95),
            'p99_wall_ms':percentile(.99),'max_wall_ms':ordered[-1],
            'percentile_method':'nearest rank',
            'scope':'RunLoop/VBlank boundaries after warmup; not physical scanout or unique game FPS.'}


def summarize_command_timeline(output, warmup, frames):
    path=output/'metal-command-timeline.csv'
    if not path.is_file():return None
    info=json.loads((output/'metal-command-timeline-info.json').read_text())
    intervals=[];rows=0
    with path.open(newline='') as stream:
        reader=csv.DictReader(stream)
        if reader.fieldnames!=['submit_refresh','retire_refresh','producer','start_seconds','end_seconds']:
            raise ValueError('Invalid command timing columns')
        for row in reader:
            rows+=1
            submit=int(row['submit_refresh']);retire=int(row['retire_refresh'])
            start=float(row['start_seconds']);end=float(row['end_seconds'])
            if submit<0 or retire<submit or not all(map(math.isfinite,(start,end))) or not 0<start<=end or end-start>=60:
                raise ValueError('Invalid command GPU timing')
            if warmup<=submit<frames:intervals.append((start,end))
    if rows!=info['retained']:raise ValueError('Incomplete retained command timing CSV')
    result={'commands':len(intervals),'missing':info['missing'],'dropped':info['dropped'],
            'complete_retained_data':not info['missing'] and not info['dropped'],
            'scope':'GPU start/end interval coverage for tracked commands submitted after warmup; gaps are not proof of CPU starvation, covered intervals are not exclusive GPU utilization.'}
    if not intervals:return result
    intervals.sort();first,last=intervals[0];covered=0;gaps=[]
    for begin,end in intervals[1:]:
        if begin>last:
            covered+=last-first;gaps.append(begin-last);first,last=begin,end
        else:last=max(last,end)
    covered+=last-first
    span=last-intervals[0][0]
    result.update(observed_gpu_timeline_seconds=span,union_covered_seconds=covered,
                  uncovered_seconds=max(0,span-covered),largest_gap_ms=max(gaps,default=0)*1000,
                  gaps=len(gaps),sum_command_seconds=sum(end-start for start,end in intervals))
    return result


def summarize_queue_wait_timeline(output, warmup, frames, waits_by_refresh=None):
    path = output / 'metal-queue-waits.csv'
    if not path.is_file():
        return None
    dropped = json.loads((output / 'metal-queue-waits-info.json').read_text())['dropped']
    columns = ['refresh_before_vblank', 'elapsed_ns', 'oldest_draw_count',
               'oldest_command_gpu_ms', 'wait_after_gpu_ms']
    legacy = ['refresh_before_vblank', 'elapsed_ns', 'oldest_command_gpu_ms']
    counts = dict(waits=0, missing_gpu_boundaries=0, dropped=dropped)
    totals = dict(before_gpu_start=0.0, during_gpu_execution=0.0,
                  after_gpu_end=0.0, unattributed=0.0)
    with path.open(newline='') as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames not in (columns, legacy):
            raise ValueError('Invalid queue wait timing columns')
        for row in reader:
            refresh = int(row['refresh_before_vblank'])
            wall = int(row['elapsed_ns']) / 1e6
            gpu = float(row['oldest_command_gpu_ms'])
            after = float(row.get('wait_after_gpu_ms', '-1'))
            if (refresh < 0 or wall < 0 or not all(map(math.isfinite, (gpu, after)))
                    or (gpu < 0 and gpu != -1) or (after < 0 and after != -1)
                    or after > wall + .0001):
                raise ValueError('Invalid queue wait timing')
            if not warmup <= refresh < frames:
                continue
            if waits_by_refresh is not None:
                # The queue event precedes VBlank; the wall interval ends at it.
                completed = refresh + 1
                waits_by_refresh[completed] = waits_by_refresh.get(completed, 0) + wall
            counts['waits'] += 1
            if gpu == -1 or after == -1:
                counts['missing_gpu_boundaries'] += 1
                totals['unattributed'] += wall
                continue
            after = min(after, wall)  # The CSV truncates wall time to integer nanoseconds.
            during = min(gpu, wall - after)
            totals['before_gpu_start'] += wall - after - during
            totals['during_gpu_execution'] += during
            totals['after_gpu_end'] += after
    return {
        **counts,
        'complete_retained_data': not dropped and not counts['missing_gpu_boundaries'],
        'total_ms_per_refresh': sum(totals.values()) / (frames - warmup),
        'split_ms_per_refresh': {name: milliseconds / (frames - warmup)
                                 for name, milliseconds in totals.items()},
        'scope': 'Host blocking waits on the oldest tracked command. GPU overlap covers '
                 'only the part of its command span inside the host wait window.',
    }


def seed_pipeline_cache(source, output):
    """Retain only locally learned cache artifacts in a fresh run container."""
    if source.is_symlink() or not source.is_dir() or source.name!='metal-pipelines':
        raise ValueError('Pipeline cache source must be a regular metal-pipelines directory')
    limits={'recipes':64*1024*1024,'msl':40*1024*1024,'metallib':128*1024*1024}
    files=[]
    for path in sorted(source.iterdir()):
        match=re.fullmatch(r'[0-9a-f]{64}(?:-[0-9a-f]{64})?\.(recipes|msl|metallib)',path.name)
        if path.is_symlink() or not path.is_file() or not match:
            raise ValueError(f'Unsupported pipeline cache entry: {path}')
        if path.stat().st_size>limits[match.group(1)]:raise ValueError(f'Oversized pipeline cache entry: {path}')
        files.append(path)
    destination=output/'metal-pipelines'
    destination.mkdir(exist_ok=False)
    records=[]
    for path in files:
        copied=destination/path.name
        copy_input_file(path,copied)
        if copied.is_symlink():raise ValueError(f'Pipeline cache entry changed to a symlink: {path}')
        records.append({'path':str(Path('metal-pipelines')/path.name),
                        'bytes':copied.stat().st_size,'sha256':file_sha256(copied)})
    raw=(json.dumps(records,sort_keys=True,indent=2)+'\n').encode()
    (output/'pipeline-cache-input-manifest.json').write_bytes(raw)
    return hashlib.sha256(raw).hexdigest()


def retain_build_audit(build, output, binary_sha256):
    """Bind a build receipt to this executable, without asserting source equality."""
    path=build/'native-metal-audit.json'
    if not path.is_file():return {'status':'missing'}
    raw=path.read_bytes()
    receipt={'path':str(path),'sha256':hashlib.sha256(raw).hexdigest()}
    try:
        audit=json.loads(raw)
        matches=isinstance(audit,dict) and audit.get('binary_sha256')==binary_sha256
    except (ValueError,UnicodeError):
        receipt['status']='invalid'
        return receipt
    if not matches:
        receipt['status']='binary_mismatch'
        return receipt
    (output/'native-metal-audit.json').write_bytes(raw)
    receipt.update(status='binary_matched',retained_file='native-metal-audit.json',
                   build_source_revision=audit.get('source_revision'),
                   build_tracked_source_modified=audit.get('tracked_source_modified'))
    return receipt


def configure_compiler_profile(output, build_audit, env):
    """Keep compiler-training writes isolated and never label training clean."""
    if build_audit.get('status') != 'binary_matched':return False
    audit=json.loads((output/'native-metal-audit.json').read_text())
    policy=audit.get('compiler_policy',{})
    pgo=policy.get('fastinterp_pgo',{}) if isinstance(policy,dict) else {}
    generation=isinstance(pgo,dict) and pgo.get('mode')=='generate'
    if generation:env['LLVM_PROFILE_FILE']=str(output/'fastinterp-%p.profraw')
    return generation


def retain_untracked_engine_sources(root, output):
    """Retain current untracked engine source omitted by git diff HEAD."""
    paths=subprocess.check_output(
        ['git','ls-files','--others','--exclude-standard','-z','--','emulator'],cwd=root)
    extensions={'.h','.hpp','.hh','.hxx','.c','.cc','.cpp','.cxx','.m','.mm','.metal','.cmake','.in','.inc'}
    records=[]
    for name in sorted(os.fsdecode(path) for path in paths.split(b'\0') if path):
        relative=Path(name)
        if relative.suffix not in extensions and relative.name!='CMakeLists.txt':continue
        source=root/relative
        # A symlink may refer outside the checkout; never follow it into a run archive.
        if source.is_symlink() or not source.is_file():
            records.append({'path':name,'status':'not_regular_file'})
            continue
        raw=source.read_bytes()
        retained=Path('source-untracked')/relative
        destination=output/retained
        destination.parent.mkdir(parents=True,exist_ok=True)
        destination.write_bytes(raw)
        records.append({'path':name,'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),
                        'retained_file':str(retained),'status':'retained'})
    return records


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom',type=Path,required=True)
    parser.add_argument('--build-dir',type=Path,default=ROOT/'work/build-native-metal-mac')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--frames',type=int,default=7200)
    parser.add_argument('--warmup-frames',type=int,default=1200)
    parser.add_argument('--inputs',type=Path,help='Existing frame-based input script; no inputs are invented')
    parser.add_argument('--state',type=Path,help='Compatible checkpoint to restore through normal runner validation; frames and inputs are relative to restore')
    parser.add_argument('--state-filesystem',type=Path,help='Seed normal savedata or a restored checkpoint; only sdmc/nand/sysdata are copied; required with --state')
    checkpoint = parser.add_mutually_exclusive_group()
    checkpoint.add_argument('--save-state-at',type=int,help='Save checkpoint.carastate at this frame boundary; clears renderer watch history')
    checkpoint.add_argument('--save-state-at-end',action='store_true',help='Save after timing and boundary snapshots; preserves normal renderer history during measurement')
    parser.add_argument('--pipeline-cache-from',type=Path,help='Prior local run metal-pipelines directory; copy only bounded learned artifacts into fresh output')
    parser.add_argument('--instrumented',action='store_true')
    parser.add_argument('--external-profile-label', help='Mark external instrumentation without enabling renderer clocks or stack sampling')
    parser.add_argument('--capture-interval',type=int,help='Diagnostic intermediate capture interval in refreshes; adds capture/readback overhead')
    parser.add_argument('--compare-to',type=Path,help='Require matching rendered pixels and retained guest boundary against a completed run')
    parser.add_argument('--scene-reference',type=Path,help='Coarse cross-build endpoint scene guard against a visually verified run; does not replace exact --compare-to')
    parser.add_argument('--fastinterp-option',action='append',default=[],metavar='CARA_FASTINTERP_NAME=VALUE')
    parser.add_argument('--metal-option',action='append',default=[],metavar='CARA_METAL_NAME=VALUE')
    parser.add_argument('--shader-option',action='append',default=[],metavar='NAME=0|1',help='Shared shader policy: CARA_GPU_QUATERNION_SLICE or CARA_GPU_PACKED_FETCH_REUSE only')
    parser.add_argument('--expected-title',default='4000000055f00',help='Hex title ID; default is Luigi Dark Moon USA')
    parser.add_argument('--cpu-backend',choices=('fastinterp','jit'),default='fastinterp',help='JIT is an explicit macOS diagnostic requiring a matching JIT-capable build audit; default FastInterp')
    args=parser.parse_args()
    if args.frames<=args.warmup_frames or args.warmup_frames<0:parser.error('Frame limit must exceed the warmup')
    if args.capture_interval is not None and args.capture_interval<=0:parser.error('Capture interval must be positive')
    if args.state and not args.state_filesystem:parser.error('--state requires --state-filesystem')
    if args.save_state_at is not None and not 0<args.save_state_at<args.frames:parser.error('Save-state boundary must satisfy 0 < boundary < frames')
    rom=args.rom.resolve();output=args.output.resolve()
    if args.state:
        if args.state.is_symlink() or not args.state.is_file():parser.error('State must be a regular checkpoint file')
    if args.state_filesystem:
        if args.state_filesystem.is_symlink() or not args.state_filesystem.is_dir():parser.error('State filesystem must be a regular directory')
        seed=args.state_filesystem.resolve()
        if output==seed or seed in output.parents:parser.error('Output must be outside the seed user directory')
    binary=args.build_dir.resolve()/'bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom'
    if not rom.is_file() or not binary.is_file():parser.error('ROM or native Metal executable missing')
    binary_sha256=file_sha256(binary)
    try:validate_cpu_backend(args.build_dir.resolve(),binary_sha256,args.cpu_backend)
    except ValueError as error:parser.error(str(error))
    if output.exists():parser.error('Choose a fresh output directory to retain previous evidence')
    output.mkdir(parents=True);(output/'config').mkdir()
    cache_manifest_sha256=None
    if args.pipeline_cache_from:
        try:cache_manifest_sha256=seed_pipeline_cache(args.pipeline_cache_from,output)
        except ValueError as error:parser.error(str(error))
    state_sha256=None;seed_manifest_sha256=None
    if args.state_filesystem:
        try:seed_manifest_sha256=seed_state_filesystem(seed,output)
        except ValueError as error:parser.error(str(error))
    if args.state:
        copy_input_file(args.state,output/'restore-state.carastate')
        if (output/'restore-state.carastate').is_symlink():parser.error('State changed to a symlink during preparation')
        state_sha256=file_sha256(output/'restore-state.carastate')
    (output/'config/sdl2-config.ini').write_text('[Core]\nuse_cpu_jit='+('true' if args.cpu_backend=='jit' else 'false')+'\ncpu_clock_percentage=100\n')
    if args.inputs:copy_input_file(args.inputs,output/'inputs.txt')
    env={key:value for key,value in os.environ.items() if not key.startswith('CARA_')}
    env.update(CARA_USER_DIR=str(output),CARA_CPU_BACKEND=args.cpu_backend,CARA_NATIVE_DIAGNOSTICS='1')
    if args.state:env['CARA_COMPAT_LOAD_STATE']=str(output/'restore-state.carastate')
    if args.save_state_at is not None:env['CARA_COMPAT_SAVE_STATE_FRAME']=str(args.save_state_at)
    if args.save_state_at_end:env['CARA_COMPAT_SAVE_STATE_AT_END']='1'
    if args.warmup_frames:
        env['CARA_METAL_BENCHMARK_WARMUP']=str(args.warmup_frames)
    if args.capture_interval is not None:
        env['CARA_COMPAT_CAPTURE_INTERVAL']=str(args.capture_interval)
    options={}
    for option in args.metal_option:
        name,separator,value=option.partition('=')
        if not separator or not name.startswith('CARA_METAL_') or not re.fullmatch(r'[A-Z0-9_]+',name):
            parser.error('Metal options must be CARA_METAL_NAME=VALUE')
        if name in ('CARA_METAL_FRAME_COST','CARA_METAL_EARLY_PROFILE'):
            parser.error('Use --instrumented for profiling counters so the report identifies their overhead')
        if name=='CARA_METAL_BENCHMARK_WARMUP':
            parser.error('Use --warmup-frames to keep runtime and reported warmup identical')
        options[name]=value
    # Learning remains explicit for reproducible bounded runs. Ordinary frontend
    # defaults are independent of this helper's measurement policy.
    options.setdefault('CARA_METAL_PIPELINE_LEARNING','0')
    cpu_options={}
    for option in args.fastinterp_option:
        name,separator,value=option.partition('=')
        if not separator or not name.startswith('CARA_FASTINTERP_') or not re.fullmatch(r'[A-Z0-9_]+',name):
            parser.error('FastInterp options must be CARA_FASTINTERP_NAME=VALUE')
        cpu_options[name]=value
    shader_options={}
    for option in args.shader_option:
        name,separator,value=option.partition('=')
        if not separator or name not in ('CARA_GPU_QUATERNION_SLICE','CARA_GPU_PACKED_FETCH_REUSE') or value not in ('0','1'):
            parser.error('Shader options must select CARA_GPU_QUATERNION_SLICE or CARA_GPU_PACKED_FETCH_REUSE with value 0 or 1')
        shader_options[name]=value
    env.update(cpu_options)
    env.update(options)
    env.update(shader_options)
    queue_wait_timeline = options.get('CARA_METAL_QUEUE_WAIT_TIMELINE') == '1'
    refresh_cpu_timeline = options.get('CARA_METAL_REFRESH_CPU_TIMELINE') == '1'
    if args.instrumented:env.update(CARA_METAL_FRAME_COST='1',CARA_METAL_EARLY_PROFILE='1')
    command=[str(binary),str(rom),str(args.frames)]
    patch=subprocess.check_output(['git','diff','HEAD','--binary'],cwd=ROOT)
    (output/'source-changes.patch').write_bytes(patch)
    binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest()
    build_audit=retain_build_audit(args.build_dir.resolve(),output,binary_sha256)
    compiler_profile_generation=configure_compiler_profile(output,build_audit,env)
    untracked_sources=retain_untracked_engine_sources(ROOT,output)
    manifest=dict(tracked_diff_sha256=hashlib.sha256(patch).hexdigest(),command=command,source_revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                  binary_sha256=binary_sha256,build_audit=build_audit,
                  current_untracked_engine_sources=untracked_sources,
                  source_provenance_scope='Revision, tracked diff and untracked engine source are snapshots at run preparation; they do not establish the source used to build this executable.',
                  untracked_source_scope='Non-ignored source/header/Metal/CMake inputs under emulator; not a complete build dependency closure.',
                  cpu_backend=args.cpu_backend,guest_cpu_percent=100,
                  compiler_profile_generation=compiler_profile_generation,
                  external_profile_label=args.external_profile_label,
                  metal_options=options,fastinterp_overrides=cpu_options,fixed_rtc=True,detailed_renderer_profile=args.instrumented,warmup_frames=args.warmup_frames,
                  shader_options=shader_options,
                  capture_interval=args.capture_interval,
                  intermediate_capture_overhead=args.capture_interval is not None,
                  state_sha256=state_sha256,state_filesystem_manifest_sha256=seed_manifest_sha256,
                  state_source=str(args.state.resolve()) if args.state else None,
                  state_filesystem_source=str(seed) if args.state_filesystem else None,
                  save_state_at=args.save_state_at,
                  save_state_at_end=args.save_state_at_end,
                  replay_frame_scope='Post-restore refreshes' if args.state else 'Normal boot refreshes',
                  pipeline_cache_source=str(args.pipeline_cache_from.resolve()) if args.pipeline_cache_from else None,
                  pipeline_cache_input_manifest_sha256=cache_manifest_sha256,
                  inputs_sha256=hashlib.sha256((output/'inputs.txt').read_bytes()).hexdigest() if args.inputs else None,
                  caveats=['Speed windows measure emulation progress, not individual frame latency or exclusive GPU time.',
                           'Startup windows are retained but excluded from the steady-window summary.',
                           'A matching binary build audit binds its recorded build metadata only; current checkout equality with built source is not established.',
                           *(['Intermediate captures add GPU completion/readback and file I/O; this is a progress diagnostic, not a clean speed comparison.'] if args.capture_interval is not None else []),
                           'A fresh user directory does not establish a cold Apple driver cache.'])
    (output/'run.json').write_text(json.dumps(manifest,indent=2)+'\n')
    started=time.monotonic();sampler=None;title=None;execution=None;result=None;host_qos=None;host_power=None;host_activity=None;cpu_policy={}
    skipped_shadow_requests=None;audio_sink_opened=False;effective_metal_options={}
    cache_reports={}
    with (output/'console.log').open('w') as log:
        process=subprocess.Popen(command,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
        try:
            for line in process.stdout:
                log.write(line);log.flush()
                plain=re.sub(r'\x1b\[[0-9;]*m','',line).strip()
                if plain.startswith(('EXECUTION ','TITLE ','PERF ','RESULT ','METAL_STOP ','HOST_ERROR ','HOST_QOS ','HOST_POWER ','HOST_ACTIVITY ','CPU_POLICY ','METAL_POLICY ')):print(plain,flush=True)
                if plain.startswith('TITLE '):title=plain.split()[1]
                if plain.startswith('EXECUTION '):execution=plain
                if plain.startswith('RESULT '):result=plain
                if plain.startswith('HOST_QOS '):host_qos=plain
                if plain.startswith('HOST_POWER '):host_power=plain
                if plain.startswith('HOST_ACTIVITY '):host_activity=plain
                if plain.startswith('CPU_POLICY '):
                    name,_,value=plain[len('CPU_POLICY '):].partition('=');cpu_policy[name]=value
                if plain.startswith('METAL_POLICY '):
                    name,_,value=plain[len('METAL_POLICY '):].partition('=');effective_metal_options[name]=value
                shadow=re.search(r'Matched iPad GLES unsupported-shadow requests: (\d+)',plain)
                if shadow:skipped_shadow_requests=int(shadow.group(1))
                if re.search(r'Opened SDL audio device \d+:',plain):audio_sink_opened=True
                for label in ('Metal learned cache device=','Metal warmup startup ',
                              'Metal learned prewarm attempted=',
                              'Metal learned MSL save ','Metal pipeline learning ',
                              'Metal warmup binary archive saved='):
                    if label in plain:cache_reports[label]=plain[plain.index(label):]
                frame=re.search(r'^PERF frame=(\d+)',plain)
                if args.instrumented and sampler is None and frame and int(frame.group(1))>=args.warmup_frames:
                    sampler=subprocess.Popen(['sample',str(process.pid),'5','-file',str(output/'host-sample.txt')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            code=process.wait()
        except BaseException:
            process.terminate();process.wait();raise
    if sampler is not None:sampler.wait()
    records=[]
    report=output/'metal-performance.jsonl'
    if report.exists():records=[json.loads(line) for line in report.read_text().splitlines() if line]
    steady=[r for r in records if r['start_refresh']>=args.warmup_frames]
    duration=sum(r['wall_seconds'] for r in steady)
    waits_by_refresh = {}
    queue_summary = summarize_queue_wait_timeline(output, args.warmup_frames, args.frames,
                                                  waits_by_refresh)
    refresh_tail = summarize_refresh_tail(
        read_refresh_intervals(output/'metal-refresh-intervals.csv', args.warmup_frames),
        read_refresh_intervals(output/'metal-refresh-cpu.csv', args.warmup_frames, 'thread_cpu_ms'),
        waits_by_refresh if queue_summary is not None and not queue_summary['dropped'] else None,
    )
    summary=dict(exit_code=code,wall_seconds_including_startup_and_shutdown=time.monotonic()-started,title=title,
                 cpu_backend=args.cpu_backend,
                 execution=execution,result=result,host_qos=host_qos,host_power=host_power,host_activity=host_activity,cpu_policy=cpu_policy,steady_windows=len(steady),
                 weighted_speed_percent=sum(r['emulation_speed_percent']*r['wall_seconds'] for r in steady)/duration if duration else None,
                 lowest_window_speed_percent=min((r['emulation_speed_percent'] for r in steady),default=None),
                 graphics_policy_allow_shadow=False,graphics_policy_shadow_rendering=False,
                 skipped_unsupported_shadow_requests=skipped_shadow_requests,
                 graphics_policy_scope='Existing iPad GLES compatibility shadow policy; comparisons do not establish full graphics coverage.',
                 audio_sink_opened=audio_sink_opened,
                 audio_scope='Observed SDL device-open log only; not proof of audible or smooth playback.',
                 effective_metal_options=effective_metal_options,
                 pipeline_cache_reports=cache_reports,
                 refresh_intervals=summarize_refresh_intervals(output/'metal-refresh-intervals.csv',args.warmup_frames),
                 command_timeline=summarize_command_timeline(output,args.warmup_frames,args.frames),
                 queue_wait_timeline=queue_summary, refresh_tail=refresh_tail,
                 cpu_cache_profile=cpu_options.get('CARA_FASTINTERP_CACHE_PROFILE')=='1',
                 cpu_ldr_pair_census=cpu_options.get('CARA_FASTINTERP_LDR_PAIR_CENSUS')=='1',
                 compiler_profile_generation=compiler_profile_generation,
                 external_profile_label=args.external_profile_label,
                 instrumented=args.instrumented or queue_wait_timeline or refresh_cpu_timeline or cpu_options.get('CARA_FASTINTERP_CACHE_PROFILE')=='1' or cpu_options.get('CARA_FASTINTERP_LDR_PAIR_CENSUS')=='1' or compiler_profile_generation or bool(args.external_profile_label))
    if args.save_state_at is not None or args.save_state_at_end:
        checkpoint=output/'checkpoint.carastate'
        summary['saved_checkpoint_sha256']=file_sha256(checkpoint) if checkpoint.is_file() else None
    capture=output/'metal-benchmark-frame.bgra'
    metadata=output/'metal-benchmark-frame.json'
    if capture.exists() and metadata.exists():
        layout=json.loads(metadata.read_text());raw=capture.read_bytes()
        width,height,stride=layout['width'],layout['height'],layout['row_stride']
        if layout['format']!='BGRA8' or stride<width*4 or len(raw)!=stride*height:
            raise SystemExit('Invalid retained framebuffer layout')
        pixels=b''.join(raw[y*stride:y*stride+width*4] for y in range(height))
        summary['pixels_sha256']=hashlib.sha256(pixels).hexdigest()
        summary['capture_layout']=layout
    boundary=output/'guest-boundary.json'
    if boundary.exists():
        summary['guest_boundary_sha256']=hashlib.sha256(boundary.read_bytes()).hexdigest()
    interval=output/'metal-measured-interval.json'
    if interval.exists():summary['measured_interval']=json.loads(interval.read_text())
    expected_result=f'RESULT frames={args.frames} presented={args.frames} captured=1'
    summary['complete_capture']=result==expected_result and 'pixels_sha256' in summary
    if args.scene_reference:
        summary['scene_reference']=check_scene_reference(output,args.scene_reference)
    if args.compare_to:
        reference=args.compare_to.resolve()
        previous=json.loads((reference/'summary.json').read_text())
        identity=json.loads((reference/'run.json').read_text())
        comparison={key:summary.get(key) is not None and summary.get(key)==previous.get(key)
                    for key in ('pixels_sha256','capture_layout','guest_boundary_sha256','cpu_policy','result')}
        comparison.update({key:manifest.get(key)==identity.get(key)
                           for key in ('binary_sha256','inputs_sha256','guest_cpu_percent','fixed_rtc','state_sha256','state_filesystem_manifest_sha256')})
        summary['comparison']=comparison
        summary['comparison_passed']=all(comparison.values())
        summary['comparison_scope']='Endpoint pixels and CPU/tick boundary under existing iPad GLES compatibility shadow policy only; not full graphics coverage, complete guest memory, audio, intermediate frames or service state.'
    (output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
    if code or not summary['complete_capture'] or (args.scene_reference and not summary['scene_reference']['passed']) or ((args.save_state_at is not None or args.save_state_at_end) and not summary['saved_checkpoint_sha256']) or (args.compare_to and not summary['comparison_passed']) or title is None or int(title,16)!=int(args.expected_title,16) or execution is None or ('backend='+args.cpu_backend+' ') not in execution or 'guest_cpu_percent=100' not in execution:
        raise SystemExit('Run failed validation; inspect retained console.log')


if __name__=='__main__':main()
