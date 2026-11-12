#!/usr/bin/env python3
"""Replay a private Cara profiling checkpoint on iOS; never add game data to Git."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bundle', default='com.dmikey.cara')
    parser.add_argument('--rom', help='Pin a ROM filename already present in the app Documents directory')
    parser.add_argument('--frames', type=int, default=1200)
    parser.add_argument('--warmup', type=int, default=300)
    parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('--config', action='append', required=True,
                        help='JSON object: name, packed (boolean), bank_mib (16..128), optional reuse and index_cache (booleans)')
    parser.add_argument('--resume-best', action='store_true')
    parser.add_argument('--screenshots', action='store_true')
    parser.add_argument('--press-a-frames', default='0',
                        help='Comma-separated emulated frames for six-frame A presses; 0 disables')
    parser.add_argument('--host-cpu-timing', action='store_true')
    parser.add_argument('--cpu-category-timing', action='store_true', help='Exclusive diagnostic thread CPU categories; not a performance comparison')
    parser.add_argument('--gpu-self-test', action='store_true',
                        help='Validate GPU fixtures before replay; abort if they fail')
    parser.add_argument('--phase-cpu-timing', action='store_true',
                        help='Diagnostic sampled phase CPU/wall totals; not an uninstrumented performance run')
    parser.add_argument('--circle-segments', default='',
                        help='Comma-separated begin:end:x:y stick movements; neutral between segments')
    args = parser.parse_args()
    if args.cpu_category_timing and args.resume_best:
        parser.error('CPU category diagnostics cannot select a performance winner')
    if args.frames <= args.warmup or args.frames % 60 or args.warmup % 60:
        parser.error('frames and warmup must be multiples of 60, with frames > warmup')
    try:
        press_frames = [int(value) for value in args.press_a_frames.split(',')]
    except ValueError:
        parser.error('press-a-frames must contain integers')
    if any(value < 0 or value >= args.frames for value in press_frames):
        parser.error('A press frames must be within the run')
    import math
    circle_segments = []
    try:
        for item in filter(None, args.circle_segments.split(',')):
            begin, end, x, y = item.split(':')
            begin, end, x, y = int(begin), int(end), float(x), float(y)
            if not (0 <= begin < end <= args.frames and math.isfinite(x) and math.isfinite(y)
                    and abs(x) <= 1 and abs(y) <= 1):
                raise ValueError('invalid circle segment')
            if circle_segments and begin < circle_segments[-1][1]:
                raise ValueError('segments must be ordered and nonoverlapping')
            circle_segments.append((begin, end, x, y))
    except ValueError:
        parser.error('circle-segments require ordered begin:end:x:y values within the run and [-1,1]')
    configs = [json.loads(value) for value in args.config]
    for config in configs:
        if (any(not isinstance(config.get(key, False), bool) for key in ['state_reuse','vertex_reuse','quaternion_slice','implicit_flush','uniform_reuse','retain_small_writes','decode_bkpt','ram_burst','lit_vertex_reuse','surface_remap','retain_input_bindings','packed_fetch_reuse','add_cmp_imm','vcmp_vmrs','lit_pair_reuse','lit_pair_probe','interactive_qos','contiguous_transfers','render_bookkeeping_reuse','render_bookkeeping_profile','texture_lookup_reuse','texture_lookup_verify']) or type(config.get('vfp_simd', 0)) not in (bool, int) or config.get('vfp_simd', 0) not in (0, 1, 2, 3) or not isinstance(config.get('index_cache', False), bool) or not isinstance(config.get('reuse', False), bool) or not isinstance(config['packed'], bool) or not isinstance(config['bank_mib'], int)
                or not 16 <= config['bank_mib'] <= 128
                or not config['name'].replace('_', '').replace('-', '').isalnum()):
            parser.error('invalid configuration')
    if len({config['name'] for config in configs}) != len(configs):
        parser.error('configuration names must be unique')
    args.output.mkdir(parents=True, exist_ok=True)
    if args.resume_best and any(c.get('render_bookkeeping_profile',False) or c.get('texture_lookup_verify',False) for c in configs):
        parser.error('Renderer diagnostics cannot select a performance winner')
    if any(c.get('texture_lookup_verify',False) and not c.get('texture_lookup_reuse',False) for c in configs):
        parser.error('Texture verification requires texture lookup reuse')
    env = os.environ.copy()
    env.setdefault('DEVELOPER_DIR', '/Volumes/Fast/Applications/Xcode.app/Contents/Developer')

    def run(command, quiet=False):
        return subprocess.run(['xcrun', 'devicectl', *command], env=env, timeout=60,
                              stdout=subprocess.DEVNULL if quiet else None,
                              stderr=subprocess.DEVNULL if quiet else None)

    def copy(name, target):
        return run(['device', 'copy', 'from', '--device', args.device,
                    '--source', 'Documents/cara/' + name, '--destination', str(target),
                    '--domain-type', 'appDataContainer', '--domain-identifier', args.bundle], True).returncode == 0

    checkpoint = args.output / 'profile-comparison.carastate'
    if not copy(checkpoint.name, checkpoint):
        raise RuntimeError('Capture a comparison checkpoint in Cara before running this tool')
    results = {'checkpoint_sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(), 'press_a_frames': press_frames, 'circle_segments': circle_segments, 'touch_input_locked': True, 'phase_cpu_sampling': args.phase_cpu_timing, 'cpu_category_timing':args.cpu_category_timing, 'runs': []}

    def launch(config, benchmark=True):
        settings = {'CARA_FRAME_TIMING': '1', 'CARA_AUDIO_DIAGNOSTICS': '1',
                    'CARA_FASTINTERP_PROFILE':'1',
                    'CARA_TEST_KEEP_AWAKE':'1',
                    'CARA_CPU_CATEGORY_TIMING':'1' if args.cpu_category_timing else '0',
                    'CARA_PHASE_CPU_TIMING':'1' if args.phase_cpu_timing else '0',
                    'CARA_BENCHMARK_CAPTURE':'1',
                    'CARA_GPU_SURFACE_REMAP':'1' if config.get('surface_remap',False) else '0',
                    'CARA_GPU_RETAIN_INPUT_BINDINGS':'1' if config.get('retain_input_bindings',False) else '0',
                    'CARA_GPU_PACKED_FETCH_REUSE':'1' if config.get('packed_fetch_reuse',False) else '0',
                    'CARA_FASTINTERP_VFP_SIMD':str(int(config.get('vfp_simd',0))),
                    'CARA_WORKER_INTERACTIVE_QOS':'1' if config.get('interactive_qos',False) else '0',
                    'CARA_GPU_LIT_PAIR_REUSE':'1' if config.get('lit_pair_reuse',False) else '0',
                    'CARA_GPU_LIT_PAIR_PROBE':'1' if config.get('lit_pair_probe',False) else '0',
                    'CARA_GPU_LIT_VERTEX_REUSE':'1' if config.get('lit_vertex_reuse',False) else '0',
                    'CARA_FASTINTERP_RAM_BURST':'1' if config.get('ram_burst',False) else '0',
                    'CARA_FASTINTERP_CONTIGUOUS_TRANSFERS':'1' if config.get('contiguous_transfers',False) else '0',
                    'CARA_RENDER_BOOKKEEPING_REUSE':'1' if config.get('render_bookkeeping_reuse',False) else '0',
                    'CARA_RENDER_BOOKKEEPING_PROFILE':'1' if config.get('render_bookkeeping_profile',False) else '0',
                    'CARA_TEXTURE_LOOKUP_REUSE':'1' if config.get('texture_lookup_reuse',False) else '0',
                    'CARA_TEXTURE_LOOKUP_VERIFY':'1' if config.get('texture_lookup_verify',False) else '0',
                    'CARA_FASTINTERP_DECODE_BKPT':'1' if config.get('decode_bkpt',False) else '0',
                    'CARA_SURFACE_FLUSH_TRACE':'1',
                    'CARA_RETAIN_SMALL_CPU_WRITES':'1' if config.get('retain_small_writes',False) else '0',
                    'CARA_GPU_QUATERNION_SLICE':'1' if config.get('quaternion_slice',False) else '0',
                    'CARA_GL_IMPLICIT_FLUSH':'1' if config.get('implicit_flush',False) else '0',
                    'CARA_FASTINTERP_VCMP_VMRS':'1' if config.get('vcmp_vmrs',False) else '0',
                    'CARA_FASTINTERP_ADD_CMP_IMM':'1' if config.get('add_cmp_imm',False) else '0',
                    'CARA_VS_UNIFORM_REUSE':'1' if config.get('uniform_reuse',False) else '0',
                    'CARA_GL_STATE_REUSE':'1' if config.get('state_reuse',False) else '0',
                    'CARA_GPU_VERTEX_REUSE':'1' if config.get('vertex_reuse',False) else '0',
                    'CARA_OPEN_RECENT': '1', 'CARA_TIMING_WINDOW_FRAMES': '60',
                    'CARA_CPU_BACKEND': 'fastinterp', 'CARA_PROFILE_LOAD': '1',
                    'CARA_TEST_PRESS_A_FRAMES': args.press_a_frames if benchmark else '0',
                    'CARA_BENCHMARK_INPUT_LOCK': '1' if benchmark else '0',
                    'CARA_TEST_CIRCLE_SEGMENTS': args.circle_segments if benchmark else '',
                    'CARA_TEST_HOLD_START_FRAME': '0',
                    'CARA_PROFILE_SAVE_FRAME': '0',
                    'CARA_GPU_PACKED_INPUTS': '1' if config['packed'] else '0',
                    'CARA_VERTEX_UPLOAD_REUSE': '1' if config.get('reuse', False) else '0',
                    'CARA_VERTEX_INDEX_CACHE': '1' if config.get('index_cache', False) else '0',
                    'CARA_VERTEX_UPLOAD_BANK_MIB': str(config['bank_mib']),
                    'CARA_ADAPTIVE_GUEST_CPU': '0' if benchmark else '1'}
        if args.gpu_self_test:
            settings['CARA_VERTEX_SELF_TEST'] = '1'
        if args.host_cpu_timing:
            settings['CARA_HOST_CPU_TIMING'] = '1'
        if benchmark:
            settings['CARA_BENCHMARK_FRAMES'] = str(args.frames)
            settings['CARA_BENCHMARK_AUDIO'] = '1'
        arguments = []
        if args.rom:
            settings.pop('CARA_OPEN_RECENT')
            arguments = ['--', '-rom', Path(args.rom).name]
        run(['device', 'process', 'launch', '--device', args.device, '--terminate-existing',
             '--environment-variables', json.dumps(settings), args.bundle, *arguments]).check_returncode()

    for config in configs:
        launch(config)
        print('Started', config['name'], flush=True)
        time.sleep(3)
        started = time.monotonic()
        target = args.output / (config['name'] + '-frame-timing.json')
        # Observe a new, incomplete run before accepting completion, so an old
        # report cannot be mistaken for the just-launched process.
        observed_start = False
        while time.monotonic() - started < args.timeout:
            if copy('frame-timing.json', target):
                try:
                    report = json.loads(target.read_text())
                    if args.rom and report.get('rom_filename') != Path(args.rom).name:
                        raise RuntimeError('Different game opened during comparison')
                    window = report['windows'][-1]
                    right_mode = ((window['gpu_packed_input_draws'] > 0) == config['packed']
                                  and window['vertex_upload_bank_budget_bytes'] == config['bank_mib'] * 1048576)
                    if right_mode and window['end_frame'] < args.frames:
                        observed_start = True
                    if observed_start and right_mode and window['end_frame'] >= args.frames:
                        break
                except (ValueError, KeyError, IndexError):
                    pass  # Report may be in the middle of a write.
            time.sleep(5)
        else:
            copy('emulator.log', args.output / (config['name'] + '-failure.log'))
            raise RuntimeError('Run did not complete: ' + config['name'])
        windows = [w for w in report['windows'] if args.warmup < w['end_frame'] <= args.frames]
        if len(windows) != (args.frames - args.warmup) // 60:
            raise RuntimeError('Incomplete measurement window')
        if any(w['guest_cpu_percent'] != 100 or w['packed_validation_failures'] != 0
               or w['packed_verified_draws'] != 0 for w in windows):
            raise RuntimeError('Unexpected clock, validation failure or active shadow decoder')
        fields = ['surface_remap_ms_per_frame','surface_remap_calls','surface_remap_bytes','gpu_feedback_vertex_draws','gpu_feedback_source_vertices','surface_pack_ms_per_frame','surface_download_ms_per_frame','surface_download_calls','surface_download_bytes','shader_compile_ms_per_frame','gpu_wait_ms_per_frame','uniform_upload_ms_per_frame','stream_map_ms_per_frame','stream_unmap_ms_per_frame','stream_map_calls','stream_unmap_calls','stream_mapped_bytes','state_apply_ms_per_frame','gpu_direct_vertex_draws','gpu_direct_vertices','gpu_direct_unique_inputs','gpu_lit_pair_draws','gpu_lit_pair_vertices','gpu_lit_pair_ids','mean_ms', 'vertex_prepare_ms_per_frame', 'vertex_upload_ms_per_frame',
                  'draw_submit_ms_per_frame', 'vertex_upload_bytes', 'gpu_vertices',
                  'gpu_vertex_draws', 'gpu_packed_input_draws', 'vertex_upload_fallbacks',
                  'vertex_upload_reused_bytes', 'vertex_upload_reused_textures',
                  'vertex_reuse_check_ms_per_frame', 'vertex_index_cache_hits',
                  'vertex_index_cache_misses', 'vertex_index_cache_bytes']
        fields += ['draw_framebuffer_acquire_ms_per_frame', 'draw_framebuffer_acquire_calls', 'draw_texture_acquire_ms_per_frame', 'draw_texture_acquire_calls', 'draw_shader_setup_ms_per_frame', 'draw_shader_setup_calls', 'draw_dirty_invalidate_ms_per_frame', 'draw_dirty_invalidate_calls', 'render_reinterpret_check_ms_per_frame', 'render_reinterpret_check_calls', 'render_reinterpret_invocations', 'render_reinterpret_find_calls', 'render_reinterpret_avoided_calls', 'render_reinterpret_repeat_opportunities', 'contiguous_transfer_loads', 'contiguous_transfer_stores', 'contiguous_transfer_bytes']
        fields += ['texture_lookup_checks', 'texture_lookup_hits', 'texture_lookup_misses', 'texture_lookup_stores', 'texture_lookup_ambiguous_rejects', 'texture_lookup_epoch_rejects', 'texture_lookup_verified_hits', 'texture_lookup_verify_mismatches']
        if args.phase_cpu_timing:
            fields += [phase+'_'+suffix for phase in ['draw_submit','vertex_orphan','stream_map','stream_unmap','uniform_upload','vertex_subimage']
                       for suffix in ['sample_wall_ms','sample_cpu_ms','samples','sample_clock_failures']]
        if args.host_cpu_timing:
            fields += ['emulation_thread_cpu_ms_per_frame', 'process_cpu_ms_per_frame']
        if any(w['texture_lookup_verify_mismatches'] for w in windows):
            raise RuntimeError('Texture lookup shadow verification failed')
        if config.get('texture_lookup_verify',False):
            if sum(w['texture_lookup_verified_hits'] for w in windows) == 0:
                raise RuntimeError('Texture lookup verification did not exercise any hits')
        elif any(w['texture_lookup_verified_hits'] for w in windows):
            raise RuntimeError('Unexpected texture shadow verification in speed comparison')
        means = {key: sum(w[key] for w in windows) / len(windows) for key in fields}
        result = dict(config, fingerprint=report['engine_build_fingerprint'], means=means)
        for name in ['runtime-status.json', 'audio-status.json', 'benchmark-status.json', 'fastinterp-blocks.json','surface-flushes.json']:
            for attempt in range(5):
                if copy(name, args.output / (config['name'] + '-' + name)):
                    break
                time.sleep(0.2)
            else:
                raise RuntimeError('Unable to read ' + name)
        if args.gpu_self_test:
            check_path = args.output / (config['name'] + '-gpu-vertex-self-test.json')
            if not copy('gpu-vertex-self-test.json', check_path) or json.loads(check_path.read_text()).get('passed') is not True:
                raise RuntimeError('GPU fixture did not pass')
        status = json.loads((args.output / (config['name'] + '-benchmark-status.json')).read_text())
        if (status['result_code'] != 0 or status['frames'] != args.frames or
                status['target_frames'] != args.frames or status['title_id'] != report['title_id'] or
                status['guest_cpu_percent'] != 100 or status['adaptive_guest_cpu']):
            raise RuntimeError('Benchmark did not complete successfully at fixed 100% clock')
        for name in ['benchmark-frame.json', 'benchmark-frame.rgba']:
            if not copy(name, args.output / (config['name'] + '-' + name)):
                raise RuntimeError('Unable to read final renderer pixels: ' + name)
        capture = json.loads((args.output / (config['name'] + '-benchmark-frame.json')).read_text())
        pixels = (args.output / (config['name'] + '-benchmark-frame.rgba')).read_bytes()
        if capture['gl_error'] or capture['frame'] != args.frames or len(pixels) != 400 * 480 * 4:
            raise RuntimeError('Invalid final renderer capture')
        result['renderer_rgba_sha256'] = hashlib.sha256(pixels).hexdigest()
        results['runs'].append(result)
        (args.output / 'comparison.json').write_text(json.dumps(results, indent=2) + '\n')
        if args.screenshots:
            run(['device', 'capture', 'screenshot', '--device', args.device,
                 '--destination', str(args.output / (config['name'] + '.png'))]).check_returncode()
        print(config['name'], round(means['mean_ms'], 3), 'ms per refresh', flush=True)
    fingerprints = {r['fingerprint'] for r in results['runs']}
    if len(fingerprints) != 1:
        raise RuntimeError('Runs used different engine builds')
    results['renderer_rgba_identical'] = len({r['renderer_rgba_sha256'] for r in results['runs']}) == 1
    results['matched_workload'] = len({(r['means']['gpu_vertices'], r['means']['gpu_vertex_draws'])
                                      for r in results['runs']}) == 1
    (args.output / 'comparison.json').write_text(json.dumps(results, indent=2) + '\n')
    if args.resume_best:
        if not results['matched_workload'] or not results['renderer_rgba_identical']:
            raise RuntimeError('Workloads or renderer pixels differ; refusing to choose a winner automatically')
        best = min(results['runs'], key=lambda r: r['means']['mean_ms'])
        launch(best, benchmark=False)
        print('Resumed adaptive play:', best['name'], flush=True)


if __name__ == '__main__':
    main()
