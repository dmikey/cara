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

    def test_absolute_pc_is_optional_boolean_configuration(self):
        tree = ast.parse(HARNESS.read_text(), filename=str(HARNESS))
        checks = [node for node in ast.walk(tree) if isinstance(node, ast.If)
                  and any(isinstance(child, ast.Call) and isinstance(child.func, ast.Attribute)
                          and isinstance(child.func.value, ast.Name)
                          and child.func.value.id == 'parser' and child.func.attr == 'error'
                          and any(isinstance(arg, ast.Constant) and arg.value == 'invalid configuration'
                                  for arg in child.args)
                          for child in ast.walk(node))]
        self.assertTrue(checks, 'configuration validation guard must exist')
        check = min(checks, key=lambda node: len(list(ast.walk(node))))
        code = compile(ast.fix_missing_locations(ast.Module(body=[check], type_ignores=[])),
                       str(HARNESS), 'exec')
        baseline = dict(name='fixture', packed=True, bank_mib=80)
        for config in (baseline, dict(baseline, absolute_pc=False), dict(baseline, absolute_pc=True)):
            parser = SimpleNamespace(error=Mock(side_effect=ValueError))
            exec(code, dict(config=config, parser=parser))
            parser.error.assert_not_called()
        for value in (0, 1, None, 'true'):
            with self.subTest(value=value):
                parser = SimpleNamespace(error=Mock(side_effect=ValueError))
                with self.assertRaises(ValueError):
                    exec(code, dict(config=dict(baseline, absolute_pc=value), parser=parser))
                parser.error.assert_called_once_with('invalid configuration')

    def test_absolute_pc_compiled_mode_never_sets_runtime_toggle(self):
        for requested in (None, False, True):
            with self.subTest(requested=requested):
                config = dict(packed=True, bank_mib=80)
                if requested is not None:
                    config['absolute_pc'] = requested
                settings, _ = self.capture(config)
                self.assertFalse(any('ABSOLUTE_PC' in key for key in settings))
                self.assert_benchmark_contract(settings)

    def test_absolute_pc_census_requires_actual_booleans_only_when_requested(self):
        message = 'Compiled absolute-PC mode is missing, invalid, or differs from requested configuration'
        tree = ast.parse(HARNESS.read_text(), filename=str(HARNESS))
        checks = [node for node in ast.walk(tree) if isinstance(node, ast.If)
                  and any(isinstance(child, ast.Constant) and child.value == message
                          for child in ast.walk(node))]
        self.assertTrue(checks, 'compiled absolute-PC census guard must exist')
        check = min(checks, key=lambda node: len(list(ast.walk(node))))
        code = compile(ast.fix_missing_locations(ast.Module(body=[check], type_ignores=[])),
                       str(HARNESS), 'exec')
        for requested in (False, True):
            good = [{'absolute_pc_enabled': requested} for _ in range(4)]
            exec(code, dict(census={'cores': good}, config={'absolute_pc': requested}))
            for index in range(4):
                for bad in ({}, {'absolute_pc_enabled': not requested},
                            {'absolute_pc_enabled': 0}, {'absolute_pc_enabled': 1},
                            {'absolute_pc_enabled': None}, {'absolute_pc_enabled': 'true'}):
                    cores = [dict(core) for core in good]
                    cores[index] = bad
                    with self.subTest(requested=requested, index=index, bad=bad):
                        with self.assertRaisesRegex(RuntimeError, message):
                            exec(code, dict(census={'cores': cores}, config={'absolute_pc': requested}))
        # Older artifact comparisons intentionally make no assertion about this
        # compiled mode unless the configuration explicitly requests one.
        for core in ({}, {'absolute_pc_enabled': False}, {'absolute_pc_enabled': True},
                     {'absolute_pc_enabled': None}, {'absolute_pc_enabled': 0}):
            exec(code, dict(census={'cores': [dict(core) for _ in range(4)]}, config={}))

    def test_cache_capacity_launch_control_defaults_to_16k(self):
        for entries in (None, 16384, 32768):
            with self.subTest(entries=entries):
                config = dict(packed=True, bank_mib=80)
                if entries is not None:
                    config['cache_entries'] = entries
                settings, _ = self.capture(config)
                self.assertEqual(settings['CARA_FASTINTERP_CACHE_ENTRIES'],
                                 str(16384 if entries is None else entries))
                self.assert_benchmark_contract(settings)

    def test_cache_census_requires_exactly_four_core_objects(self):
        message = 'FastInterp census must contain exactly four core objects'
        tree = ast.parse(HARNESS.read_text(), filename=str(HARNESS))
        checks = [node for node in ast.walk(tree) if isinstance(node, ast.If)
                  and any(isinstance(child, ast.Constant) and child.value == message
                          for child in ast.walk(node))]
        self.assertTrue(checks, 'census shape guard must exist')
        check = min(checks, key=lambda node: len(list(ast.walk(node))))
        code = compile(ast.fix_missing_locations(ast.Module(body=[check], type_ignores=[])),
                       str(HARNESS), 'exec')
        exec(code, dict(census={'cores': [{} for _ in range(4)]}))
        invalid = [None, [], {}, {'cores': None}, {'cores': {}}, {'cores': 'four'},
                   {'cores': ()}, {'cores': []}, {'cores': [{} for _ in range(3)]},
                   {'cores': [{} for _ in range(5)]}]
        for index in range(4):
            cores = [{} for _ in range(4)]
            cores[index] = None
            invalid.append({'cores': cores})
        for census in invalid:
            with self.subTest(census=census):
                with self.assertRaisesRegex(RuntimeError, message):
                    exec(code, dict(census=census))

    def test_cache_capacity_census_requires_every_core_to_match(self):
        # Execute the real guard, including its default, rather than a copy of
        # its comparison. A healthy core must not hide another core's report.
        message = 'FastInterp capacity differs from requested configuration'
        tree = ast.parse(HARNESS.read_text(), filename=str(HARNESS))
        checks = [node for node in ast.walk(tree) if isinstance(node, ast.If)
                  and any(isinstance(child, ast.Constant) and child.value == message
                          for child in ast.walk(node))]
        self.assertTrue(checks, 'capacity census guard must exist')
        check = min(checks, key=lambda node: len(list(ast.walk(node))))
        code = compile(ast.fix_missing_locations(ast.Module(body=[check], type_ignores=[])),
                       str(HARNESS), 'exec')
        for requested in (None, 16384, 32768):
            config = {} if requested is None else {'cache_entries': requested}
            expected = 16384 if requested is None else requested
            good = [{'cache_entries': expected} for _ in range(4)]
            with self.subTest(requested=requested, case='four matching cores'):
                exec(code, dict(census={'cores': good}, config=config))
            for index in range(4):
                for bad in ({}, {'cache_entries': None},
                            {'cache_entries': 32768 if expected == 16384 else 16384}):
                    cores = [dict(core) for core in good]
                    cores[index] = bad
                    with self.subTest(requested=requested, index=index, bad=bad):
                        with self.assertRaisesRegex(RuntimeError, message):
                            exec(code, dict(census={'cores': cores}, config=config))

    def test_depth_alias_and_presence_based_self_tests_are_independent(self):
        for alias in (None, False, True):
            for self_test in (None, False, True):
                for gpu_self_test in (False, True):
                    with self.subTest(alias=alias, self_test=self_test,
                                      gpu_self_test=gpu_self_test):
                        config = dict(packed=True, bank_mib=80)
                        if alias is not None:
                            config['depth_reinterpret'] = alias
                        if self_test is not None:
                            config['depth_self_test'] = self_test
                        settings, _ = self.capture(config, gpu_self_test=gpu_self_test)
                        self.assertEqual(settings['CARA_GPU_DEPTH_REINTERPRET'],
                                         '1' if alias else '0')
                        # The app tests getenv presence; a disabled flag with
                        # value "0" would still run these expensive fixtures.
                        for key in ('CARA_GPU_DEPTH_SELF_TEST', 'CARA_SURFACE_SELF_TEST'):
                            self.assertEqual(key in settings, bool(self_test))
                            if self_test:
                                self.assertEqual(settings[key], '1')
                        self.assertEqual('CARA_VERTEX_SELF_TEST' in settings, gpu_self_test)
                        self.assert_benchmark_contract(settings)

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
