#!/usr/bin/env python3
"""Replay a private Cara profiling checkpoint on iOS; never add game data to Git."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
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
    parser.add_argument('--paced', action='store_true', help='Cap emulated time at 100%%; wall timings include pacing sleep')
    parser.add_argument('--memory-probes', help='Read-only diagnostic JSON probes: address and optional pointer_offset (at most eight)')
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
    memory_probes=None
    if args.memory_probes:
        try:
            memory_probes=json.loads(args.memory_probes)
            if not isinstance(memory_probes,list) or not 1<=len(memory_probes)<=8:
                raise ValueError()
            for probe in memory_probes:
                if not isinstance(probe,dict) or 'address' not in probe or set(probe)-{'address','pointer_offset'}:
                    raise ValueError()
                for value in probe.values():
                    if isinstance(value,str): value=int(value,16 if value.lower().startswith('0x') else 10)
                    if type(value) is not int or not 0<=value<=0xffffffff: raise ValueError()
        except (ValueError,TypeError):
            parser.error('memory-probes requires 1..8 unsigned 32-bit address/pointer_offset objects')
    if args.resume_best and (args.paced or memory_probes):
        parser.error('Paced or memory-probe diagnostics cannot select a performance winner')
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
        if (any(not isinstance(config.get(key, False), bool) for key in ['state_reuse','vertex_reuse','quaternion_slice','implicit_flush','uniform_reuse','retain_small_writes','decode_bkpt','ram_burst','lit_vertex_reuse','surface_remap','byte_surface_remap','retain_input_bindings','packed_fetch_reuse','add_cmp_imm','vcmp_vmrs','lit_pair_reuse','lit_pair_probe','interactive_qos','contiguous_transfers','render_bookkeeping_reuse','render_bookkeeping_profile','texture_lookup_reuse','texture_lookup_verify','alu_operands','alu_pairs','tick_buckets','chain_budget','cache_profile','draw_cost_profile','pica_repeat_state','pica_command_profile','pica_post_irq_profile','experimental_p3d_completion','experimental_ppf_dependency','ios_pipeline_cache','bounded_pacing']) or type(config.get('cache_ways', 4)) is not int or config.get('cache_ways', 4) not in (4, 8) or type(config.get('vfp_simd', 0)) not in (bool, int) or config.get('vfp_simd', 0) not in (0, 1, 2, 3) or not isinstance(config.get('index_cache', False), bool) or not isinstance(config.get('reuse', False), bool) or not isinstance(config['packed'], bool) or not isinstance(config['bank_mib'], int)
                or not 16 <= config['bank_mib'] <= 128
                or not config['name'].replace('_', '').replace('-', '').isalnum()):
            parser.error('invalid configuration')
    if any(c.get('experimental_ppf_dependency', False) and not c.get('experimental_p3d_completion', False) for c in configs):
        parser.error('PPF dependency requires experimental P3D completion')
    if len({config['name'] for config in configs}) != len(configs):
        parser.error('configuration names must be unique')
    args.output.mkdir(parents=True, exist_ok=True)
    if args.resume_best and any(c.get('draw_cost_profile',False) or c.get('cache_profile',False) or c.get('pica_command_profile',False) or c.get('pica_post_irq_profile',False) or c.get('experimental_p3d_completion',False) or c.get('experimental_ppf_dependency',False) or c.get('render_bookkeeping_profile',False) or c.get('texture_lookup_verify',False) for c in configs):
        parser.error('Renderer diagnostics cannot select a performance winner')
    if any(type(c.get('guest_cpu_percent',100)) is not int or c.get('guest_cpu_percent',100) not in (50,100) for c in configs):
        parser.error('Diagnostic guest clock must be 50 or 100 percent')
    if args.resume_best and any(c.get('guest_cpu_percent',100) != 100 for c in configs):
        parser.error('Underclock diagnostics cannot select a performance winner')
    if any(c.get('alu_pairs',False) and not c.get('alu_operands',False) for c in configs):
        parser.error('ALU pairs require operand specialization')
    if any(c.get('cache_ways',4) == 8 and not c.get('chain_budget',False) for c in configs):
        parser.error('Eight-way comparisons require cache-independent chain budget checks')
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
    checkpoint_bytes = checkpoint.read_bytes()
    if len(checkpoint_bytes) < 112 or checkpoint_bytes[:8] != b'CARAST01':
        raise RuntimeError('Invalid comparison checkpoint header')
    checkpoint_title = struct.unpack_from('<Q', checkpoint_bytes, 16)[0]
    checkpoint_fingerprint = checkpoint_bytes[48:112].decode('ascii')
    results = {'checkpoint_sha256': hashlib.sha256(checkpoint_bytes).hexdigest(), 'press_a_frames': press_frames, 'circle_segments': circle_segments, 'touch_input_locked': True, 'phase_cpu_sampling': args.phase_cpu_timing, 'cpu_category_timing':args.cpu_category_timing, 'paced':args.paced, 'memory_probes':memory_probes, 'runs': []}

    def launch(config, benchmark=True):
        settings = {'CARA_FRAME_TIMING': '1', 'CARA_AUDIO_DIAGNOSTICS': '1',
                    'CARA_FASTINTERP_PROFILE':'1',
                    'CARA_TEST_KEEP_AWAKE':'1',
                    'CARA_CPU_CATEGORY_TIMING':'1' if args.cpu_category_timing else '0',
                    'CARA_PHASE_CPU_TIMING':'1' if args.phase_cpu_timing else '0',
                    'CARA_BENCHMARK_CAPTURE':'1',
                    'CARA_GPU_SURFACE_REMAP':'1' if config.get('surface_remap',False) else '0',
                    'CARA_GPU_BYTE_SURFACE_REMAP':'1' if config.get('byte_surface_remap',False) else '0',
                    'CARA_GPU_RETAIN_INPUT_BINDINGS':'1' if config.get('retain_input_bindings',False) else '0',
                    'CARA_GPU_PACKED_FETCH_REUSE':'1' if config.get('packed_fetch_reuse',False) else '0',
                    'CARA_FASTINTERP_VFP_SIMD':str(int(config.get('vfp_simd',0))),
                    'CARA_WORKER_INTERACTIVE_QOS':'1' if config.get('interactive_qos',False) else '0',
                    'CARA_GPU_LIT_PAIR_REUSE':'1' if config.get('lit_pair_reuse',False) else '0',
                    'CARA_GPU_LIT_PAIR_PROBE':'1' if config.get('lit_pair_probe',False) else '0',
                    'CARA_GPU_LIT_VERTEX_REUSE':'1' if config.get('lit_vertex_reuse',False) else '0',
                    'CARA_FASTINTERP_RAM_BURST':'1' if config.get('ram_burst',False) else '0',
                    'CARA_FASTINTERP_TICK_BUCKETS':'1' if config.get('tick_buckets',False) else '0',
                    'CARA_FASTINTERP_CHAIN_BUDGET':'1' if config.get('chain_budget',False) else '0',
                    'CARA_FASTINTERP_CACHE_WAYS':str(config.get('cache_ways',4)),
                    'CARA_DRAW_COST_PROFILE':'1' if config.get('draw_cost_profile',False) else '0',
                    'CARA_FASTINTERP_CACHE_PROFILE':'1' if config.get('cache_profile',False) else '0',
                    'CARA_FASTINTERP_ALU_OPERANDS':'1' if config.get('alu_operands',False) else '0',
                    'CARA_FASTINTERP_ALU_PAIRS':'1' if config.get('alu_pairs',False) else '0',
                    'CARA_PROFILE_GUEST_CPU_PERCENT':str(config.get('guest_cpu_percent',100)),
                    'CARA_PICA_REPEAT_STATE':'1' if config.get('pica_repeat_state',False) else '0',
                    'CARA_PICA_COMMAND_PROFILE':'1' if config.get('pica_command_profile',False) else '0',
                    'CARA_PICA_POST_IRQ_PROFILE':'1' if config.get('pica_post_irq_profile',False) else '0',
                    'CARA_EXPERIMENTAL_PPF_DEPENDENCY':'1' if config.get('experimental_ppf_dependency',False) else '0',
                    'CARA_EXPERIMENTAL_P3D_COMPLETION':'1' if config.get('experimental_p3d_completion',False) else '0',
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
        if config.get('ios_pipeline_cache', False):
            settings['CARA_IOS_PIPELINE_CACHE']='1'
        settings['CARA_BOUNDED_FRAME_PACING']='1' if config.get('bounded_pacing',False) else '0'
        if benchmark and args.paced:
            settings['CARA_PACED_BENCHMARK']='1'
            settings['CARA_MAX_SPEED_PERCENT']='100'
        if benchmark and memory_probes:
            settings['CARA_PROFILE_MEMORY_U32']=json.dumps(memory_probes)
            settings['CARA_PROFILE_MEMORY_U32_FRAME_300']='1'
        settings['CARA_AUDIO_NO_STRETCH']='1'
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
        if any(w['guest_cpu_percent'] != config.get('guest_cpu_percent',100) or w['packed_validation_failures'] != 0
               or w['packed_verified_draws'] != 0 for w in windows):
            raise RuntimeError('Unexpected clock, validation failure or active shadow decoder')
        fields = ['surface_remap_ms_per_frame','surface_remap_calls','surface_remap_bytes','gpu_feedback_vertex_draws','gpu_feedback_source_vertices','surface_pack_ms_per_frame','surface_download_ms_per_frame','surface_read_pixels_ms_per_frame','surface_read_pixels_calls','surface_download_calls','surface_download_bytes','shader_compile_ms_per_frame','gpu_wait_ms_per_frame','uniform_upload_ms_per_frame','stream_map_ms_per_frame','stream_unmap_ms_per_frame','stream_map_calls','stream_unmap_calls','stream_mapped_bytes','state_apply_ms_per_frame','gpu_direct_vertex_draws','gpu_direct_vertices','gpu_direct_unique_inputs','gpu_lit_pair_draws','gpu_lit_pair_vertices','gpu_lit_pair_ids','mean_ms', 'vertex_prepare_ms_per_frame', 'vertex_upload_ms_per_frame',
                  'draw_submit_ms_per_frame', 'vertex_upload_bytes', 'gpu_vertices',
                  'gpu_vertex_draws', 'gpu_packed_input_draws', 'vertex_upload_fallbacks',
                  'vertex_upload_reused_bytes', 'vertex_upload_reused_textures',
                  'vertex_reuse_check_ms_per_frame', 'vertex_index_cache_hits',
                  'vertex_index_cache_misses', 'vertex_index_cache_bytes']
        fields += ['draw_framebuffer_acquire_ms_per_frame', 'draw_framebuffer_acquire_calls', 'draw_texture_acquire_ms_per_frame', 'draw_texture_acquire_calls', 'draw_shader_setup_ms_per_frame', 'draw_shader_setup_calls', 'draw_dirty_invalidate_ms_per_frame', 'draw_dirty_invalidate_calls', 'render_reinterpret_check_ms_per_frame', 'render_reinterpret_check_calls', 'render_reinterpret_invocations', 'render_reinterpret_find_calls', 'render_reinterpret_avoided_calls', 'render_reinterpret_repeat_opportunities', 'contiguous_transfer_loads', 'contiguous_transfer_stores', 'contiguous_transfer_bytes']
        fields += ['pica_command_lists','pica_draw_requests','top_buffer_swaps','stereo_top_buffer_swaps','pica_post_irq_submissions','pica_post_irq_writes','pica_post_irq_draw_requests','pica_post_irq_jumps','pica_first_irq_remaining_words','experimental_p3d_scheduled','experimental_p3d_delivered','experimental_p3d_retired','experimental_p3d_modeled_ns','experimental_p3d_queued_ticks']
        fields += ['guest_cpu_ms_per_frame','draw_ms_per_frame','cpu_shader_ms_per_frame','shader_calls','cpu_surface_invalidations','vertex_gl_subimage_calls','vertex_gl_image_calls','vertex_index_upload_bytes','gpu_input_records','lit_draws','active_light_sum']
        fields += ['texture_lookup_checks', 'texture_lookup_hits', 'texture_lookup_misses', 'texture_lookup_stores', 'texture_lookup_ambiguous_rejects', 'texture_lookup_epoch_rejects', 'texture_lookup_verified_hits', 'texture_lookup_verify_mismatches']
        if args.phase_cpu_timing:
            fields += [phase+'_'+suffix for phase in ['draw_submit','vertex_orphan','stream_map','stream_unmap','uniform_upload','vertex_subimage','surface_download','surface_read_pixels']
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
        for name in ['runtime-status.json', 'audio-status.json', 'benchmark-status.json', 'fastinterp-blocks.json','surface-flushes.json','emulator.log']:
            for attempt in range(5):
                if copy(name, args.output / (config['name'] + '-' + name)):
                    break
                time.sleep(0.2)
            else:
                raise RuntimeError('Unable to read ' + name)
        if memory_probes:
            probe_path=args.output / (config['name']+'-profile-memory-u32.json')
            if not copy('profile-memory-u32.json',probe_path):
                raise RuntimeError('Missing fresh memory probe report')
            probe_report=json.loads(probe_path.read_text())
            if (probe_report.get('title_id') != report['title_id'] or
                    probe_report.get('engine_build_fingerprint') != report['engine_build_fingerprint'] or
                    probe_report.get('read_only') is not True or not probe_report.get('samples')):
                raise RuntimeError('Memory probe report identity or safety mode differs')
            if probe_report.get('configuration_error'):
                raise RuntimeError('Memory probe rejected: '+probe_report['configuration_error'])
        if args.gpu_self_test:
            check_path = args.output / (config['name'] + '-gpu-vertex-self-test.json')
            if not copy('gpu-vertex-self-test.json', check_path) or json.loads(check_path.read_text()).get('passed') is not True:
                raise RuntimeError('GPU fixture did not pass')
        census = json.loads((args.output / (config['name'] + '-fastinterp-blocks.json')).read_text())
        alu_enabled = config.get('alu_operands', False)
        if any(core.get('alu_operands_enabled') != alu_enabled for core in census['cores']):
            raise RuntimeError('FastInterp operand specialization mode differs from requested configuration')
        specialized_slots = sum(sum(core.get('alu_resident_entry_weighted_slots', {}).values())
                                for core in census['cores'])
        if alu_enabled and specialized_slots <= 0:
            raise RuntimeError('FastInterp operand specialization did not execute')
        result['alu_resident_entry_weighted_slots'] = specialized_slots
        pairs_enabled=config.get('alu_pairs',False)
        if any(core.get('alu_pairs_enabled',False) != pairs_enabled for core in census['cores']):
            raise RuntimeError('FastInterp pair mode differs from requested configuration')
        pair_dispatches=sum(sum(core.get('alu_pair_resident_entry_weighted_dispatches',{}).values()) for core in census['cores'])
        if pairs_enabled and pair_dispatches <= 0:
            raise RuntimeError('FastInterp pairs did not execute')
        result['alu_pair_resident_entry_weighted_dispatches']=pair_dispatches
        if any(core.get('cache_profile_enabled') != config.get('cache_profile', False) for core in census['cores']):
            raise RuntimeError('FastInterp cache diagnostic mode differs from requested configuration')
        if any(core.get('chain_budget_enabled') != config.get('chain_budget', False) for core in census['cores']):
            raise RuntimeError('FastInterp chain budget mode differs from requested configuration')
        if any(core.get('cache_ways') != config.get('cache_ways', 4) for core in census['cores']):
            raise RuntimeError('FastInterp cache associativity differs from requested configuration')
        result['fastinterp_cache'] = [
            {key: value for key, value in core.items() if key in ('core', 'blocks_decoded', 'resident_entries') or key.startswith('cache_')}
            for core in census['cores']]
        status = json.loads((args.output / (config['name'] + '-benchmark-status.json')).read_text())
        if config.get('experimental_ppf_dependency', False) and not config.get('experimental_p3d_completion', False):
            raise RuntimeError('PPF dependency requires experimental P3D completion')
        if status.get('experimental_ppf_dependency', False) != config.get('experimental_ppf_dependency', False):
            raise RuntimeError('Experimental PPF dependency mode differs from configuration')
        if status.get('experimental_p3d_completion', False) != config.get('experimental_p3d_completion', False):
            raise RuntimeError('Experimental P3D completion mode differs from configuration')
        if status.get('paused_seconds', 0) > 0.001:
            raise RuntimeError('Replay was interrupted or paused; discard timing and workload comparison')
        if (report['title_id'] != checkpoint_title or
                report['engine_build_fingerprint'] != checkpoint_fingerprint):
            raise RuntimeError('Comparison checkpoint title or engine build differs from replay')
        audio = json.loads((args.output / (config['name'] + '-audio-status.json')).read_text())
        if (audio.get('muted') is not False or audio.get('queue_running') is not True or
                audio.get('last_error') != 0 or audio.get('nonzero_frames', 0) <= 0):
            raise RuntimeError('Benchmark audio is muted, stopped, silent or reports an output error')
        result['audio'] = {key: audio.get(key) for key in
                           ['muted', 'queue_running', 'last_error', 'nonzero_frames',
                            'dsp_underrun_callbacks', 'callbacks', 'sample_rate']}
        if (status['cpu_backend'] != 'fastinterp' or status['jit'] or status['software_renderer'] or
                status['gpu_backend'] != 'native_gles3' or status['result_code'] != 0 or status['frames'] != args.frames or
                status['target_frames'] != args.frames or status['title_id'] != report['title_id'] or
                status['guest_cpu_percent'] != config.get('guest_cpu_percent',100) or status['adaptive_guest_cpu']):
            raise RuntimeError('Benchmark did not complete successfully at the requested fixed guest clock')
        if status.get('bounded_frame_pacing',False) != config.get('bounded_pacing',False):
            raise RuntimeError('Frame pacing policy differs from requested configuration')
        if bool(status.get('paced',False)) != args.paced or (args.paced and status.get('maximum_speed_percent') != 100):
            raise RuntimeError('Replay pacing differs from the requested mode')
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
