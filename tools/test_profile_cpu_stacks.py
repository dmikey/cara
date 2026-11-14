#!/usr/bin/env python3
"""ROM-free profiler parser/attachment checks. Never invoke Instruments or a game."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import profile_cpu_stacks as tool

UUID = '11111111-2222-3333-4444-555555555555'


def fixture(rows=None, schema='time-profile'):
    rows = rows or [(71, '0x1010', 1000000), (71, '0x1020', 2000000), (71, '0x1010', 3000000)]
    body = []
    for index, (pid, pc, weight) in enumerate(rows):
        body.append(f'''<row><thread fmt="emu-{pid}"><process><pid>{pid}</pid></process></thread>
          <weight>{weight}</weight><backtrace><frame id="f{index}" name="ExecuteBlock" addr="{pc}">
          <binary name="cara" UUID="{UUID}" arch="arm64" load-addr="0x1000" path="/tmp/cara"/>
          </frame></backtrace></row>''')
    return f'<root><schema name="{schema}"/>{"".join(body)}</root>'


class ParseTests(unittest.TestCase):
    def parse(self, xml, **kwargs):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            source = Path(directory) / 'input.xml'
            source.write_text(xml)
            return tool.summarize(source, Path(directory) / 'output.json', **kwargs)

    def test_same_function_keeps_weighted_pcs_and_merges_duplicate_xml_ids(self):
        result = self.parse(fixture())
        self.assertEqual(result['weight_unit'], 'nanoseconds')
        thread = result['threads'][0]
        self.assertEqual(len(thread['self']), 1)
        self.assertEqual([(x['pc'], x['weight']) for x in thread['leaf_pcs']],
                         [('0x1010', 4000000), ('0x1020', 2000000)])

    def test_all_leaf_pcs_retained_beyond_old_rank_limit(self):
        result = self.parse(fixture([(71, hex(4096 + i * 4), 1) for i in range(120)]))
        self.assertEqual(len(result['threads'][0]['leaf_pcs']), 120)
        self.assertEqual(len(result['threads'][0]['self_locations']), 80)

    def test_local_pid_filters_other_processes(self):
        result = self.parse(fixture([(71, '0x1010', 1), (72, '0x1020', 9)]), expected_pid=71)
        self.assertEqual(result['samples'], 1)
        self.assertEqual(result['excluded_other_process_rows'], 1)
        with self.assertRaisesRegex(ValueError, 'verify sample process PID'):
            self.parse(fixture().replace('<pid>71</pid>', ''), expected_pid=71)

    def test_counter_schema_and_cycle_weights_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Only time-profile'):
            self.parse(fixture(schema='counters-profile'))
        with self.assertRaisesRegex(ValueError, 'Cycle weights'):
            self.parse(fixture().replace('<weight>', '<weight fmt="1 cycle">'))
        with self.assertRaisesRegex(ValueError, 'Negative'):
            self.parse(fixture([(71, '0x1010', -1)]))

    def test_missing_address_is_counted_not_invented(self):
        result = self.parse(fixture().replace(' addr="0x1010"', ''))
        self.assertEqual(result['threads'][0]['missing_leaf_pc_samples'], 2)
        self.assertEqual(len(result['threads'][0]['leaf_pcs']), 1)

    def test_reference_resolution_preserves_binary_identity(self):
        xml = f'''<root><schema name="time-profile"/>
        <thread id="t" fmt="emu"><process><pid>71</pid></process></thread>
        <weight id="w">250</weight><binary id="b" name="cara" UUID="{UUID}" arch="arm64"/>
        <frame id="f" name="ExecuteBlock" addr="0x1010"><binary ref="b"/></frame>
        <tagged-backtrace id="s"><frame ref="f"/></tagged-backtrace>
        <row><thread ref="t"/><weight ref="w"/><tagged-backtrace ref="s"/></row></root>'''
        result = self.parse(xml, expected_pid=71)
        self.assertEqual(result['threads'][0]['leaf_pcs'][0]['binary']['UUID'], UUID)
        self.assertEqual(result['threads'][0]['weight'], 250)

    def test_uuid_and_architecture_required(self):
        report = self.parse(fixture())
        identity = {'images': [{'uuid': UUID, 'architecture': 'arm64'}]}
        self.assertEqual(tool.verify_trace_binary(report, identity), identity['images'])
        for bad in [{'uuid': UUID, 'architecture': 'arm64e'},
                    {'uuid': 'DEADBEEF', 'architecture': 'arm64'}]:
            with self.assertRaisesRegex(ValueError, 'unverified'):
                tool.verify_trace_binary(report, {'images': [bad]})

    def test_weight_missing_rows_not_mixed_with_time(self):
        with self.assertRaisesRegex(ValueError, 'Mixed weighted'):
            self.parse(fixture().replace('<weight>1000000</weight>', ''))
        report = self.parse(fixture([(71, '0x1010', 1)]).replace('<weight>1</weight>', ''))
        self.assertEqual(report['weight_unit'], 'samples')


class CaptureTests(unittest.TestCase):
    def test_argument_modes_are_explicit(self):
        common = ['--output', '/tmp/no-run']
        args = tool.parse_args(['--local-pid', '71', '--binary', '/tmp/cara', *common])
        self.assertEqual(args.local_pid, 71)
        for invalid in [['--local-pid', '0', '--binary', '/tmp/cara'],
                        ['--local-pid', '71'], ['--pid', '71'],
                        ['--local-pid', '71', '--binary', '/tmp/cara', '--device', 'ipad'],
                        ['--xml', '/tmp/in.xml', '--schema', 'counters-profile']]:
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                tool.parse_args([*invalid, *common])
        self.assertEqual(tool.parse_args(['--pid', '71', '--device', 'ipad', *common]).pid, 71)

    def test_executable_path_mismatch_rejected_before_attachment(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'cara'
            binary.write_bytes(b'fixture')
            with mock.patch.object(tool, 'local_process_identity', return_value={
                    'pid': 71, 'executable': '/different/cara', 'start_time_ps': 'start'}), \
                 mock.patch.object(tool.subprocess, 'run') as external:
                with self.assertRaisesRegex(ValueError, 'does not identify'):
                    tool.main(['--local-pid', '71', '--binary', str(binary),
                               '--output', str(Path(directory) / 'capture')])
                external.assert_not_called()

    def test_binary_hash_and_uuid_are_derived_from_archived_file(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'cara'
            binary.write_bytes(b'abc')
            result = subprocess.CompletedProcess([], 0, stdout=f'UUID: {UUID} (arm64) cara\n')
            with mock.patch.object(tool.subprocess, 'run', return_value=result):
                identity = tool.binary_identity(binary, {})
            self.assertEqual(identity['sha256'],
                             'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')
            self.assertEqual(identity['images'], [{'uuid': UUID, 'architecture': 'arm64'}])

    def test_native_capture_commands_and_receipt_without_instruments(self):
        self.run_capture()

    def test_changed_pid_identity_fails_and_preserves_receipt(self):
        self.run_capture(changed=True)

    def run_capture(self, changed=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            binary = root / 'cara'
            binary.write_bytes(b'fixture-not-an-executable')
            output = root / 'capture'
            process = {'pid': 71, 'executable': str(binary), 'start_time_ps': 'start1'}
            after = dict(process, start_time_ps='start2') if changed else process
            identity = {'path': str(output / 'target-binary'), 'sha256': 'fixture-sha',
                        'images': [{'uuid': UUID, 'architecture': 'arm64'}]}
            calls = []

            def run(command, **kwargs):
                calls.append(command)
                if command[2] == 'record':
                    Path(command[command.index('--output') + 1]).mkdir()
                elif '--toc' in command:
                    Path(command[-1]).write_text('<trace-toc><table schema="time-profile"/></trace-toc>')
                elif '--xpath' in command:
                    Path(command[-1]).write_text(fixture())
                else:
                    self.fail(f'unexpected external command {command}')
                return subprocess.CompletedProcess(command, 0)

            with mock.patch.object(tool, 'local_process_identity', side_effect=[process, process, after]), \
                 mock.patch.object(tool, 'binary_identity', return_value=identity), \
                 mock.patch.object(tool.subprocess, 'run', side_effect=run), \
                 contextlib.redirect_stdout(io.StringIO()):
                arguments = ['--local-pid', '71', '--binary', str(binary), '--output', str(output),
                             '--source-fingerprint', 'caller-declared', '--duration', '20']
                if changed:
                    with self.assertRaisesRegex(ValueError, 'identity changed'):
                        tool.main(arguments)
                else:
                    tool.main(arguments)
            capture = calls[0]
            self.assertIn('--attach', capture)
            self.assertNotIn('--device', capture)
            self.assertNotIn('--launch', capture)
            self.assertEqual(capture[capture.index('--attach') + 1], '71')
            receipt = json.loads((output / 'capture-receipt.json').read_text())
            self.assertEqual(receipt['status'], 'failed' if changed else 'complete')
            self.assertTrue((output / 'recording.trace').exists())
            self.assertEqual((output / 'target-binary').read_bytes(), binary.read_bytes())
            self.assertEqual(receipt['source_fingerprint_verification'], 'caller_supplied_not_independently_verified')


if __name__ == '__main__':
    unittest.main()
