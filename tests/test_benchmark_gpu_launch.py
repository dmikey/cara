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
    values = dict(cpu_category_timing=False, phase_cpu_timing=False, paced=True,
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
