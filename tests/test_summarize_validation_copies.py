"""Strict totals and uncertainty handling for bounded surface-copy reports."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / 'tools/summarize_validation_copies.py'
SPEC = importlib.util.spec_from_file_location('copy_summary', TOOL)
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)


def zero():
    return dict.fromkeys(SUMMARY.TOTAL_FIELDS, 0)


def event(format=0, wall=10, cpu=5):
    surface = dict(format=format, type=0, tiled=True, width=8, height=8, stride=8,
                   scale=1, custom=False, address=4096, end=4352, handle=2,
                   registered=True, allocation_id=1, created_frame=0,
                   last_observed_invalidation_serial=1, invalidation_reason=2,
                   invalidation_frame=299, invalidation_start=4096, invalidation_end=4160)
    return dict(invocation=1, fragment=1, frame=300, requested=[4096, 4352],
                invalid=[4096, 4160], actual=[4096, 4160], guest_bytes=64, fill=False,
                source_valid_before_copy=True, src=surface, dst=copy.deepcopy(surface),
                src_scaled_rect=[0, 8, 8, 0], dst_scaled_rect=[0, 8, 8, 0],
                wall_ns=wall, thread_cpu_ns=cpu)


def total(events):
    rows = []
    for e in events:
        row = zero()
        row.update(calls=1, guest_bytes=e['guest_bytes'])
        for field, samples, failures in [('wall_ns', 'wall_samples', 'wall_failures'),
                                         ('thread_cpu_ns', 'cpu_samples', 'cpu_failures')]:
            row[samples if e[field] is not None else failures] = 1
            row[field] = e[field] or 0
        rows.append(row)
    return SUMMARY.add_totals(rows)


def fixture():
    a, b, c, d = event(wall=10, cpu=5), event(wall=None, cpu=0), event(1, 30, None), event(2, 40, 20)
    return dict(kind='returned_validate_surface_copies', enabled=True, schema_version=1,
                common_counters_required=False, written_frame=1200,
                window_start_inclusive=300, window_end_exclusive=1200,
                frame_clock='VideoCore::GetCurrentFrame', actual_min_frame=300, actual_max_frame=300,
                detail_capacity=2, identity_capacity=2, bucket_capacity=2,
                allocation_events=2, untracked_allocations=0, unknown_identity_uses=0,
                validation_invocations_all_frames=4, invalidation_calls_all_frames=1,
                totals=total([a, b, c, d]), detail_overflow=total([c, d]),
                bucket_overflow=total([d]), details=[a, b], buckets=[
                    dict(fill=False, src=SUMMARY.layout(a['src']), dst=SUMMARY.layout(a['dst']), totals=total([a, b])),
                    dict(fill=False, src=SUMMARY.layout(c['src']), dst=SUMMARY.layout(c['dst']), totals=total([c]))])


def fixture_v2():
    report = fixture()
    report['schema_version'] = 2
    report['unknown_gpu_owner_events'] = 1
    draw = dict(mode=3, scale=1, shadow=False, color_write=True, depth_write=False,
                has_stencil=False, stencil_test=False, depth_test=False, depth_mask=False,
                depth_surface_write=False, damage_narrowing_active=False,
                color_mask=15, stencil_mask=0, viewport_damage=[0, 0, 8, 8],
                actual_damage=[0, 0, 8, 8], shader_scissor=[16, 16, 20, 20])
    for event in report['details']:
        for side in ('src', 'dst'):
            s = event[side]
            observed = dict(serial=1, frame=299, range=[4096, 4160], origin=2,
                            gpu_owner=True, owner_allocation_id=1, owner=copy.deepcopy(s),
                            draw=copy.deepcopy(draw))
            s['last_invalidation_event'] = copy.deepcopy(observed)
            s['last_owner_write'] = copy.deepcopy(observed)
    row = dict(origin=2, eligibility=6, gpu_owner=True,
               owner_layout=SUMMARY.layout(report['details'][0]['src']),
               totals=dict(calls=1, actual_interval_bytes=256, hypothetical_interval_bytes=0,
                           hypothetical_reduction_bytes=256, integer_saturations=0))
    cpu = dict(origin=1, eligibility=0, gpu_owner=False, owner_layout=None,
               totals=dict(calls=1, actual_interval_bytes=8, hypothetical_interval_bytes=8,
                           hypothetical_reduction_bytes=0, integer_saturations=0))
    overflow = dict(calls=1, actual_interval_bytes=128, hypothetical_interval_bytes=128,
                    hypothetical_reduction_bytes=0, integer_saturations=0)
    total = {k: row['totals'][k] + cpu['totals'][k] + overflow[k] for k in SUMMARY.DAMAGE_FIELDS}
    report['hypothetical_damage'] = dict(
        scope='top_level_invalidation_logical_intervals_not_copy_traffic',
        totals=total, overflow=overflow, buckets=[row, cpu], actual_min_frame=300, actual_max_frame=300)
    return report


class CopySummaryTests(unittest.TestCase):
    def test_v1_and_v2_keep_copy_and_hypothetical_denominators_separate(self):
        self.assertNotIn('hypothetical_damage', SUMMARY.summarize(fixture()))
        result = SUMMARY.summarize(fixture_v2())
        self.assertEqual(result['totals']['calls'], 4)
        self.assertEqual(result['hypothetical_damage']['totals']['calls'], 3)
        self.assertEqual(result['hypothetical_damage']['totals']['hypothetical_reduction_bytes'], 256)
        self.assertIn('not copied bytes', result['hypothetical_damage']['limitation'])
        self.assertFalse(result['chronological_details']['unbiased_sample'])

    def test_v2_rejects_bad_owner_serial_draw_and_missing_provenance(self):
        for mutation in ('missing', 'serial', 'owner', 'origin', 'draw'):
            report = fixture_v2()
            s = report['details'][0]['src']
            if mutation == 'missing': del s['last_owner_write']
            elif mutation == 'serial': s['last_invalidation_event']['serial'] = 2
            elif mutation == 'owner': s['last_owner_write']['owner_allocation_id'] = 2
            elif mutation == 'origin': s['last_owner_write']['origin'] = 1
            else: s['last_owner_write']['draw']['depth_mask'] = 1
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                SUMMARY.summarize(report)

    def test_v2_hypothetical_bucket_overflow_saturation_and_eligibility(self):
        for mutation in ('arithmetic', 'counter', 'capacity', 'saturated', 'ineligible', 'cpu', 'duplicate'):
            report = fixture_v2()
            ledger = report['hypothetical_damage']
            if mutation == 'arithmetic': ledger['totals']['hypothetical_interval_bytes'] += 1
            elif mutation == 'counter': ledger['totals']['calls'] += 1
            elif mutation == 'capacity': ledger['buckets'].pop()
            elif mutation == 'saturated': ledger['totals']['integer_saturations'] = 1
            elif mutation == 'ineligible': ledger['buckets'][0]['eligibility'] = 7
            elif mutation == 'cpu': ledger['buckets'][1]['origin'] = 2
            else: ledger['buckets'][1] = copy.deepcopy(ledger['buckets'][0])
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                SUMMARY.summarize(report)

    def test_whole_window_ranking_keeps_overflow_and_clock_failures(self):
        result = SUMMARY.summarize(fixture())
        self.assertEqual(result['totals']['calls'], 4)
        self.assertEqual(result['totals']['wall_samples'], 3)
        self.assertEqual(result['totals']['wall_failures'], 1)
        self.assertEqual(result['totals']['cpu_failures'], 1)
        self.assertEqual(result['rows'][0]['src']['format'], 1)
        self.assertEqual(result['bucket_overflow']['calls'], 1)
        self.assertEqual(result['detail_overflow']['calls'], 2)
        self.assertFalse(result['chronological_details']['unbiased_sample'])
        self.assertNotIn('fps', result)
        self.assertNotIn('per_frame', result)
        self.assertEqual(SUMMARY.summarize(fixture(), rank='calls')['rows'][0]['calls'], 2)
        self.assertEqual(SUMMARY.summarize(fixture(), rank='cpu', limit=1)['rows'][0]['thread_cpu_ns'], 5)

    def test_empty_and_zero_clock_samples_are_valid(self):
        report = fixture()
        report.update(totals=zero(), details=[], buckets=[], detail_overflow=zero(),
                      bucket_overflow=zero(), actual_min_frame=None, actual_max_frame=None)
        self.assertEqual(SUMMARY.summarize(report)['rows'], [])
        self.assertEqual(SUMMARY.summarize(fixture())['totals']['cpu_samples'], 3)

    def test_details_and_buckets_must_each_reconcile_every_counter(self):
        for container in ['totals', 'detail_overflow', 'bucket_overflow']:
            for field in SUMMARY.TOTAL_FIELDS:
                with self.subTest(container=container, field=field):
                    report = fixture()
                    report[container][field] += 1
                    with self.assertRaises(ValueError):
                        SUMMARY.summarize(report)

    def test_false_valid_source_is_reported_not_silenced(self):
        report = fixture()
        report['details'][0]['source_valid_before_copy'] = False
        result = SUMMARY.summarize(report)
        self.assertEqual(result['chronological_details']['invalid_sources_before_copy'], 1)

    def test_unknown_identity_and_invalidation_are_preserved(self):
        report = fixture()
        report['details'][0]['dst'].update(allocation_id=None, created_frame=None,
            last_observed_invalidation_serial=None, invalidation_reason=0)
        result = SUMMARY.summarize(report)
        self.assertEqual(result['chronological_details']['destination_last_observed_reason'][0], 1)

    def test_invalid_fields_and_nonprogress_are_rejected(self):
        for key, value in [('frame', 1200), ('frame', True), ('guest_bytes', 63),
                           ('actual', [4160, 4224]), ('requested', [4100, 4352]),
                           ('actual', [4096, 4096]), ('thread_cpu_ns', -1),
                           ('source_valid_before_copy', 1), ('fragment', 0)]:
            report = fixture()
            report['details'][0][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                SUMMARY.summarize(report)
        for field, value in [('address', 4097), ('end', 4128), ('allocation_id', 0),
                             ('created_frame', None), ('invalidation_reason', 0),
                             ('registered', 1), ('stride', -1)]:
            report = fixture()
            report['details'][0]['dst'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                SUMMARY.summarize(report)

    def test_schema_capacities_and_duplicate_layouts_are_rejected(self):
        for key, value in [('schema_version', True), ('schema_version', 3), ('detail_capacity', 0),
                           ('common_counters_required', True), ('frame_clock', 'common'),
                           ('actual_min_frame', None), ('bucket_capacity', 1)]:
            report = fixture()
            report[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                SUMMARY.summarize(report)
        report = fixture()
        report['buckets'][1]['src'] = report['buckets'][0]['src']
        report['buckets'][1]['dst'] = report['buckets'][0]['dst']
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            SUMMARY.summarize(report)
        for key in ('details', 'buckets'):
            report = fixture()
            report[key][0] = None
            with self.assertRaises(ValueError):
                SUMMARY.summarize(report)

    def test_fill_has_no_source_texture_rectangle(self):
        report = fixture()
        report['details'][0].update(fill=True, src_scaled_rect=None)
        SUMMARY.summarize(report)
        report['details'][0]['src_scaled_rect'] = [0, 8, 8, 0]
        with self.assertRaisesRegex(ValueError, 'fill source'):
            SUMMARY.summarize(report)

    def test_cli_and_duplicate_json_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / 'trace.json', Path(directory) / 'summary.json'
            source.write_text(json.dumps(fixture()))
            result = subprocess.run([sys.executable, str(TOOL), str(source), '--output', str(output)],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(output.read_text())['totals']['calls'], 4)
            source.write_text('{"kind":"x","kind":"y"}')
            result = subprocess.run([sys.executable, str(TOOL), str(source)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('duplicate JSON key', result.stderr)
            self.assertNotIn('Traceback', result.stderr)


if __name__ == '__main__':
    unittest.main()
