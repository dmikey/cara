"""Execute the actual nested launch helper with a mocked device runner."""
import ast
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

HARNESS = Path(__file__).resolve().parents[1] / 'tools/benchmark_gpu_inputs.py'


def actual_launch(args, runner, memory_probes=None):
    # Extract only launch so importing the test cannot parse CLI arguments,
    # copy a checkpoint, sleep, launch a device process, or poll device reports.
    tree = ast.parse(HARNESS.read_text(), filename=str(HARNESS))
    main = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'main')
    matches = [node for node in main.body if isinstance(node, ast.FunctionDef) and node.name == 'launch']
    if len(matches) != 1:
        raise AssertionError('expected exactly one nested launch helper')
    module = ast.Module(body=matches, type_ignores=[])
    namespace = dict(args=args, run=runner, memory_probes=memory_probes, json=json, Path=Path)
    exec(compile(ast.fix_missing_locations(module), str(HARNESS), 'exec'), namespace)
    return namespace['launch']


def arguments(**changes):
    values = dict(warmup=300, guest_pc_episode_start=None, guest_pc_episode_end=None, guest_pc_ranges='', guest_pc_start_frame=300, guest_pc_window_frames=4,
                  guest_pc_period_frames=120, guest_pc_windows=4, cpu_category_timing=False, phase_cpu_timing=False, paced=True,
                  gpu_self_test=False, host_cpu_timing=False, frames=1200,
                  press_a_frames='0', circle_segments='', rom=None,
                  device='mock-device', bundle='test.cara')
    values.update(changes)
    return SimpleNamespace(**values)


class BenchmarkLaunchTests(unittest.TestCase):
    def capture(self, config, benchmark=True, **arg_changes):
        runner = Mock()
        actual_launch(arguments(**arg_changes), runner)(config, benchmark=benchmark)
        runner.assert_called_once()
        runner.return_value.check_returncode.assert_called_once_with()
        command = runner.call_args.args[0]
        self.assertEqual(command[:4], ['device', 'process', 'launch', '--device'])
        self.assertEqual(command[4:6], ['mock-device', '--terminate-existing'])
        env_index = command.index('--environment-variables')
        settings = json.loads(command[env_index + 1])
        self.assertTrue(all(isinstance(value, str) for value in settings.values()))
        self.assertEqual(command[env_index + 2], 'test.cara')
        return settings, command

    def assert_benchmark_contract(self, settings):
        self.assertEqual(settings['CARA_CPU_BACKEND'], 'fastinterp')
        self.assertEqual(settings['CARA_PROFILE_GUEST_CPU_PERCENT'], '100')
        self.assertEqual(settings['CARA_ADAPTIVE_GUEST_CPU'], '0')
        self.assertEqual(settings['CARA_PACED_BENCHMARK'], '1')
        self.assertEqual(settings['CARA_MAX_SPEED_PERCENT'], '100')
        self.assertEqual(settings['CARA_BENCHMARK_AUDIO'], '1')
        self.assertEqual(settings['CARA_AUDIO_NO_STRETCH'], '1')
        self.assertEqual(settings['CARA_BENCHMARK_FRAMES'], '1200')
        self.assertEqual(settings['CARA_BENCHMARK_INPUT_LOCK'], '1')

    def test_independent_byte_remap_cleanup_combinations(self):
        for byte_remap in (False, True):
            for cleanup in (False, True):
                with self.subTest(byte_remap=byte_remap, cleanup=cleanup):
                    config = dict(name='fixture', packed=True, bank_mib=80,
                                  surface_remap=True, byte_surface_remap=byte_remap,
                                  selective_surface_cleanup=cleanup, guest_cpu_percent=100)
                    settings, _ = self.capture(config)
                    self.assertEqual(settings['CARA_GPU_SURFACE_REMAP'], '1')
                    self.assertEqual(settings['CARA_GPU_BYTE_SURFACE_REMAP'], str(int(byte_remap)))
                    self.assertEqual(settings['CARA_SELECTIVE_SURFACE_CLEANUP'], str(int(cleanup)))
                    self.assert_benchmark_contract(settings)

    def test_optional_config_defaults_explicitly_disable_both(self):
        settings, _ = self.capture(dict(packed=True, bank_mib=80))
        self.assertEqual(settings['CARA_GPU_SURFACE_REMAP'], '0')
        self.assertEqual(settings['CARA_GPU_BYTE_SURFACE_REMAP'], '0')
        self.assertEqual(settings['CARA_SELECTIVE_SURFACE_CLEANUP'], '0')
        self.assertEqual(settings['CARA_BOUNDED_FRAME_PACING'], '0')
        self.assertEqual(settings['CARA_TEXTURE_EXACT_PREFILTER'], '0')
        self.assertEqual(settings['CARA_TEXTURE_ACQUISITION_PROFILE'], '0')
        self.assert_benchmark_contract(settings)

    def test_texture_prefilter_and_sampling_flags_are_independent(self):
        for prefilter in (False, True):
            for profile in (False, True):
                with self.subTest(prefilter=prefilter, profile=profile):
                    settings, _ = self.capture(dict(packed=True, bank_mib=80,
                        texture_exact_prefilter=prefilter, texture_acquisition_profile=profile))
                    self.assertEqual(settings['CARA_TEXTURE_EXACT_PREFILTER'], str(int(prefilter)))
                    self.assertEqual(settings['CARA_TEXTURE_ACQUISITION_PROFILE'], str(int(profile)))
                    self.assert_benchmark_contract(settings)

    def test_guest_pc_profiler_is_bounded_and_opt_in(self):
        settings, _ = self.capture(dict(packed=True, bank_mib=80))
        self.assertEqual(settings['CARA_GUEST_PC_PROFILE_RANGES'], '')
        settings, _ = self.capture(dict(packed=True, bank_mib=80, guest_pc_profile=True),
                                  guest_pc_ranges='fixture=0x1000:0x1100')
        self.assertEqual(settings['CARA_GUEST_PC_PROFILE_RANGES'], 'fixture=0x1000:0x1100')
        self.assertEqual(settings['CARA_GUEST_PC_PROFILE_START_FRAME'], '300')
        self.assertEqual(settings['CARA_GUEST_PC_PROFILE_WINDOW_FRAMES'], '4')
        self.assertEqual(settings['CARA_GUEST_PC_PROFILE_PERIOD_FRAMES'], '120')
        self.assertEqual(settings['CARA_GUEST_PC_PROFILE_WINDOWS'], '4')
        self.assert_benchmark_contract(settings)

    def test_episode_fences_only_reach_diagnostic_run(self):
        args = dict(guest_pc_ranges='fixture=0x1000:0x1100',
                    guest_pc_episode_start=0x1000, guest_pc_episode_end=0x1100)
        for enabled in (False, True):
            settings, _ = self.capture(dict(packed=True, bank_mib=80, guest_pc_profile=enabled), **args)
            self.assertEqual('CARA_GUEST_PC_PROFILE_EPISODE_START' in settings, enabled)
            if enabled:
                self.assertEqual(settings['CARA_GUEST_PC_PROFILE_EPISODE_START'], '4096')
                self.assertEqual(settings['CARA_GUEST_PC_PROFILE_EPISODE_END'], '4352')
            self.assert_benchmark_contract(settings)

    def test_precise_block_end_is_explicit_and_opt_in(self):
        for enabled in (None, False, True):
            config = dict(packed=True, bank_mib=80)
            if enabled is not None:
                config['precise_block_end'] = enabled
            settings, _ = self.capture(config)
            self.assertEqual(settings['CARA_FASTINTERP_PRECISE_BLOCK_END'], '1' if enabled else '0')
            self.assert_benchmark_contract(settings)

    def test_chain_recency_is_opt_in(self):
        for enabled in (None, False, True):
            config = dict(packed=True, bank_mib=80, chain_budget=True)
            if enabled is not None:
                config['chain_recency'] = enabled
            settings, _ = self.capture(config)
            self.assertEqual(settings['CARA_FASTINTERP_CHAIN_RECENCY'], '1' if enabled else '0')
            self.assertEqual(settings['CARA_FASTINTERP_CHAIN_BUDGET'], '1')
            self.assert_benchmark_contract(settings)

    def test_cache_report_mode_is_verified_and_required(self):
        tree = ast.parse(HARNESS.read_text())
        checks = [node for node in ast.walk(tree) if isinstance(node, ast.If)
                  and any(isinstance(child, ast.Constant)
                          and child.value == 'Chain-aware replacement mode differs from requested configuration'
                          for child in ast.walk(node))]
        check = min(checks, key=lambda node: len(list(ast.walk(node))))
        code = compile(ast.fix_missing_locations(ast.Module(body=[check], type_ignores=[])),
                       str(HARNESS), 'exec')
        for expected in (False, True):
            exec(code, dict(census={'cores': [{'cache_chain_recency': expected}]},
                            config={'chain_recency': expected}))
            for bad_core in ({'cache_chain_recency': not expected}, {}):
                with self.assertRaises(RuntimeError):
                    exec(code, dict(census={'cores': [bad_core]},
                                    config={'chain_recency': expected}))

    def test_used_input_staging_is_opt_in(self):
        for enabled in (None, False, True):
            config = dict(packed=True, bank_mib=80)
            if enabled is not None:
                config['used_input_attributes'] = enabled
            settings, _ = self.capture(config)
            self.assertEqual(settings['CARA_GPU_USED_INPUTS'], '1' if enabled else '0')
            self.assert_benchmark_contract(settings)

    def test_unpaced_run_does_not_claim_speed_cap(self):
        settings, _ = self.capture(dict(packed=True, bank_mib=80), paced=False)
        self.assertNotIn('CARA_PACED_BENCHMARK', settings)
        self.assertNotIn('CARA_MAX_SPEED_PERCENT', settings)
        self.assertEqual(settings['CARA_PROFILE_GUEST_CPU_PERCENT'], '100')
        self.assertEqual(settings['CARA_BENCHMARK_AUDIO'], '1')

    def test_resume_and_rom_argument_preserve_independent_switches(self):
        config = dict(packed=True, bank_mib=80, byte_surface_remap=True,
                      selective_surface_cleanup=False)
        settings, command = self.capture(config, benchmark=False, rom='/private/fixtures/game.3ds')
        self.assertEqual(settings['CARA_GPU_BYTE_SURFACE_REMAP'], '1')
        self.assertEqual(settings['CARA_SELECTIVE_SURFACE_CLEANUP'], '0')
        self.assertNotIn('CARA_OPEN_RECENT', settings)
        self.assertEqual(command[-3:], ['--', '-rom', 'game.3ds'])
        self.assertNotIn('CARA_BENCHMARK_FRAMES', settings)
        self.assertNotIn('CARA_BENCHMARK_AUDIO', settings)
        self.assertNotIn('CARA_PACED_BENCHMARK', settings)
        self.assertEqual(settings['CARA_BENCHMARK_INPUT_LOCK'], '0')


if __name__ == '__main__':
    unittest.main()
