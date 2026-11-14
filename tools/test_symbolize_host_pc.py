#!/usr/bin/env python3
"""Focused offline analyzer checks; no Apple tools or device required."""
import argparse
import contextlib
import io
import json
import pathlib
import struct
import tempfile
import unittest
from unittest import mock

import symbolize_host_pc as tool

UUID = '11111111-2222-3333-4444-555555555555'
OTHER = 'AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE'
VM = 0x100000000
HEADER = 0x120000000


def macho(subtype=0):
    segments = []
    for name, address, protection in [('__TEXT', VM, 1), ('__TEXT_EXEC', VM+0x4000, 5), ('__DATA', VM+0x8000, 3)]:
        segments.append(struct.pack('<II16sQQQQiiII', 0x19, 72, name.encode(), address,
                                    0x1000, 0, 0, 7, protection, 0, 0))
    commands = b''.join(segments)
    return struct.pack('<IIIIIIII', 0xFEEDFACF, 0x0100000C, subtype, 2, 3, len(commands), 0, 0) + commands


def capture():
    def sample(pc, state=1):
        return dict(pc=pc, lr=0, sp=0, fp=0, before_ns=200, after_ns=210, suspend_ns=5,
                    natural_run_state=state, info_result=0, suspend_result=0, state_result=0, resume_result=0)
    return dict(images=[dict(uuid=UUID, runtime_header=HEADER, slide=HEADER-VM),
                        dict(uuid=OTHER, runtime_header=0x90000000, slide=0)],
                samples=[sample(HEADER+0x4100), sample(HEADER+0x100), sample(0x90000100, 3)],
                start_ns=100, end_ns=300, start_unix_ns=1000, capacity=6000, max_seconds=60,
                stop_reason='stopped', sampler_preflight_result=0, dropped_records=0,
                info_failures=0, suspend_failures=0, state_failures=0, resume_failures=0)


class AnalyzerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.binary = pathlib.Path(self.tmp.name)/'app'
        self.binary.write_bytes(macho())
        self.report = pathlib.Path(self.tmp.name)/'capture.json'
        self.args = argparse.Namespace(report=str(self.report), binary=str(self.binary),
                                       dsym=None, expected_uuid=None)

    def analyze(self, data=None, run=None):
        self.report.write_text(json.dumps(data if data is not None else capture()))
        def fake(*args):
            if args[1:3] == ('dwarfdump', '--uuid'):
                return f'UUID: {UUID} (arm64) {args[-1]}\n'
            self.assertEqual(args[1], 'atos')
            # Only the executable app address reaches atos, never __TEXT/data or another image.
            self.assertEqual(args[-1], hex(HEADER+0x4100))
            return 'handler (in app) (interpreter.cpp:123)\n'
        with mock.patch.object(tool, 'run', side_effect=run or fake):
            return tool.analyze(self.args)

    def test_exact_exec_segment_only_and_population_denominators(self):
        result = self.analyze()
        all_pc, running = result['populations']
        self.assertEqual((all_pc['valid_samples'], all_pc['app_samples']), (3, 1))
        self.assertEqual((running['valid_samples'], running['app_samples']), (2, 1))
        self.assertEqual(all_pc['non_app_or_unknown_samples'], 2)
        self.assertEqual(running['app_symbols'][0]['percent_of_population'], 50)
        self.assertIn('interpreter.cpp:123', all_pc['app_symbols'][0]['symbol'])
        self.assertTrue(all(result['quality']['checks'].values()))

    def test_uuid_mismatch_rejected(self):
        data = capture(); data['images'][0]['uuid'] = OTHER
        with self.assertRaisesRegex(ValueError, 'exact binary UUID'):
            self.analyze(data)

    def test_expected_uuid_mismatch_rejected(self):
        self.args.expected_uuid = OTHER
        with self.assertRaisesRegex(ValueError, 'expected-uuid'):
            self.analyze()

    def test_dsym_same_uuid_wrong_arch_rejected(self):
        with mock.patch.object(tool, 'uuids', return_value={UUID:'arm64e'}):
            with self.assertRaisesRegex(ValueError, 'UUID and architecture'):
                tool.symbols(self.binary, pathlib.Path(self.tmp.name)/'debug', 'arm64', UUID, HEADER, [])

    def test_ambiguous_uuid_arch_rejected(self):
        output = f'UUID: {UUID} (arm64) app\nUUID: {UUID} (arm64e) app\n'
        with mock.patch.object(tool, 'run', return_value=output):
            with self.assertRaisesRegex(ValueError, 'multiple architectures'):
                tool.uuids('app')

    def test_slide_mismatch_rejected(self):
        data = capture(); data['images'][0]['slide'] += 1
        with self.assertRaisesRegex(ValueError, 'slide disagree'):
            self.analyze(data)

    def test_malformed_report_and_sample_rejected(self):
        for bad in [[], {'images':[], 'samples':'bad'}]:
            with self.subTest(bad=bad), self.assertRaisesRegex(ValueError, 'object|array'):
                self.analyze(bad)
        data = capture(); data['samples'][0]['pc'] = None
        with self.assertRaisesRegex(ValueError, r'samples\[0\].pc must be an integer'):
            self.analyze(data)

    def test_clock_failure_excluded_and_resume_failure_visible(self):
        data = capture()
        data['samples'][0]['after_ns'] = 199
        data['samples'][1]['resume_result'] = 5
        data['resume_failures'] = 1
        result = self.analyze(data)
        self.assertEqual(result['quality']['valid_count'], 1)
        self.assertFalse(result['quality']['checks']['all_sample_clocks_valid'])
        self.assertFalse(result['quality']['checks']['resume_balanced'])
        self.assertTrue(result['quality']['checks']['failure_counters_agree'])

    def test_thin_arch_mismatch_rejected(self):
        self.binary.write_bytes(macho(subtype=2))
        with self.assertRaisesRegex(ValueError, 'architecture disagrees'):
            tool.macho_segments(self.binary, 'arm64')

    def test_truncated_load_command_region_rejected(self):
        self.binary.write_bytes(macho()[:-1])
        with self.assertRaisesRegex(ValueError, 'load-command region'):
            tool.macho_segments(self.binary, 'arm64')

    def test_segment_command_too_small_rejected(self):
        data = bytearray(macho()); struct.pack_into('<I', data, 36, 8)
        self.binary.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'truncated Mach-O segment'):
            tool.macho_segments(self.binary, 'arm64')

    def test_fat_slice_bounds_rejected(self):
        self.binary.write_bytes(struct.pack('>IIIIIII', 0xCAFEBABE, 1, 0x0100000C, 0, 0x100, 0x200, 0))
        with self.assertRaisesRegex(ValueError, 'fat slice'):
            tool.macho_segments(self.binary, 'arm64')

    def test_cli_readable_malformed_error(self):
        self.report.write_text('[]')
        err = io.StringIO()
        with mock.patch('sys.argv', ['symbolize_host_pc.py', str(self.report), '--binary',str(self.binary), '--output',str(self.report)+'.out']), contextlib.redirect_stderr(err):
            self.assertEqual(tool.main(), 1)
        self.assertIn('ERROR: capture JSON must be an object',err.getvalue())
        self.assertNotIn('Traceback',err.getvalue())


if __name__ == '__main__':
    unittest.main()
