"""Original generated fixtures for the offline whole-trace summarizer."""
import copy
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location(
    'surface_summary', Path(__file__).resolve().parents[1] / 'tools/summarize_surface_flushes.py')
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)


def fixture():
    source = dict(address=4096, size=192, format=1, width=8, height=8,
                  stride=8, tiled=False, scale=1)
    destination = dict(source, format=0, size=256, tiled=True)
    entry = dict(reason=1, requested_bytes=256, surface_address=4096, format=1,
                 width=8, height=8, calls=2, readback_bytes=512, seconds=.006,
                 wait_seconds=0, read_seconds=0, pack_seconds=.001,
                 source=source, destination=destination, copy_reject_bits=3)
    return dict(wait_instrumented=False, reasons=['external', 'validation'], entries=[entry])


class SurfaceSummaryTests(unittest.TestCase):
    def test_group_ranks_and_explicit_whole_trace_normalization(self):
        report = fixture()
        report['entries'].append(dict(report['entries'][0], requested_bytes=128, calls=1))
        other = copy.deepcopy(report['entries'][0])
        other.update(reason=0, calls=10, readback_bytes=2048, seconds=.001)
        other.pop('destination')
        report['entries'].append(other)
        result = SUMMARY.summarize(report)
        self.assertEqual(result['groups'], 2)
        self.assertEqual(result['totals']['calls'], 13)
        self.assertEqual(result['totals']['readback_bytes'], 3072)
        row = result['rows'][0]
        self.assertEqual(row['reason'], 'validation')
        self.assertEqual(row['wall_ms'], 12)
        self.assertEqual(row['request_sizes_bytes'], [128, 256])
        self.assertEqual(row['source']['end_exclusive'], 4288)
        self.assertNotIn('normalization', result)
        self.assertNotIn('per_frame', row)
        for rank in ('calls', 'bytes'):
            self.assertEqual(SUMMARY.summarize(report, rank=rank, limit=1)['rows'][0]['reason'], 'external')
        normalized = SUMMARY.summarize(report, frames=100)
        self.assertEqual(normalized['totals']['per_frame']['calls'], .13)
        self.assertEqual(normalized['normalization']['basis'], 'user_asserted_matching_whole_trace_count')

    def test_uninstrumented_zero_is_unavailable(self):
        result = SUMMARY.summarize(fixture())
        self.assertIsNone(result['totals']['wait_ms'])
        self.assertIsNone(result['rows'][0]['read_ms'])
        self.assertEqual(result['rows'][0]['pack_ms'], 1)
        self.assertIn('unavailable', result['wait_read_status'])
        report = fixture()
        report['wait_instrumented'] = True
        result = SUMMARY.summarize(report)
        self.assertEqual(result['totals']['wait_ms'], 0)
        self.assertEqual(result['totals']['read_ms'], 0)
        self.assertEqual(result['wait_read_status'], 'available')
        del report['entries'][0]['read_seconds']
        self.assertEqual(SUMMARY.summarize(report)['wait_read_status'], 'incomplete_fields')

    def test_dimensions_layout_and_ranges_remain_distinct(self):
        report = fixture()
        for key, value in [('format', 0), ('stride', 16), ('tiled', True), ('address', 8192)]:
            entry = copy.deepcopy(report['entries'][0])
            entry['source'][key] = value
            report['entries'].append(entry)
        entry = copy.deepcopy(report['entries'][0])
        entry['destination']['address'] = 12288
        report['entries'].append(entry)
        self.assertEqual(SUMMARY.summarize(report)['groups'], 6)

    def test_overflow_is_mixed_and_legacy_metadata_unknown(self):
        report = fixture()
        overflow = dict(report['entries'][0], reason=0, format=0xffffffff,
                        surface_address=0, requested_bytes=0, width=0, height=0)
        report['entries'].append(overflow)
        result = SUMMARY.summarize(report)
        row = next(row for row in result['rows'] if row['reason'] == 'overflow_mixed')
        self.assertIsNone(row['source'])
        self.assertIsNone(row['destination'])
        self.assertEqual(row['request_sizes_bytes'], [])
        del report['entries'][0]['source']
        row = next(row for row in SUMMARY.summarize(report)['rows'] if row['reason'] == 'validation')
        self.assertEqual(row['source']['allocation_metadata'], 'unavailable')

    def test_invalid_fields_rejected(self):
        for key, value in [('calls', True), ('readback_bytes', -1), ('seconds', float('nan'))]:
            report = fixture()
            report['entries'][0][key] = value
            with self.assertRaises(ValueError):
                SUMMARY.summarize(report)
        for frames in (0, -1, True, 1.5):
            with self.assertRaises(ValueError):
                SUMMARY.summarize(fixture(), frames=frames)


if __name__ == '__main__':
    unittest.main()
