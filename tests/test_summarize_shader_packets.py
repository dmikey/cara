"""Synthetic log protocol tests; no ROM, renderer or shader payload inputs."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

packets = module('shader_packet_summary', 'summarize_shader_packets.py')
legacy = module('legacy_command_summary', 'summarize_pica_commands.py')

# kind, grouped, declared extras, complete, capacity, eligible, exclusive, mask.
# Eligibility is explicit fixture data, not calculated by the parser under test.
COHORT = [
    ('program', 0, 2, 1, 1, 1, 0, 15),
    ('program', 0, 3, 1, 1, 1, 1, 5),
    ('program', 0, 512, 0, 1, 0, 1, 15),
    ('program', 0, 2047, 1, 0, 0, 0, 0),
    ('program', 0, 1, 1, 1, 0, 0, 8),
    ('program', 1, 8, 1, 1, 0, 1, 15),
    ('swizzle', 0, 4, 1, 1, 1, 0, 1),
    ('swizzle', 0, 0, 1, 1, 0, 1, 2),
    ('swizzle', 1, 0, 0, 1, 0, 0, 0),
]
NAMES = ('packets', 'extra_words', 'complete_packets', 'repeated_capacity_packets',
         'eligible_packets', 'eligible_extra_words')
BOUNDS = ((0, 0), (1, 1), (2, 3), (4, 7), (8, 15), (16, 31),
          (32, 63), (64, 127), (128, 511), (512, 2047))

def snapshot(number, cohort=(), all_headers=None, session=41):
    rows = [f'prefix PICA VS packet begin schema=1 session={session} snapshot={number} '
            f'all_packets={len(cohort) if all_headers is None else all_headers}']
    for kind in ('program', 'swizzle'):
        for group in (0, 1):
            selected = [p for p in cohort if p[:2] == (kind, group)]
            def counts(values):
                return dict(zip(NAMES, (len(values), sum(p[2] for p in values),
                    sum(p[3] for p in values), sum(p[4] for p in values),
                    sum(p[5] for p in values), sum(p[2] for p in values if p[5]))))
            def text(values):
                return ' '.join(f'{k}={v}' for k, v in values.items())
            prefix = f'session={session} snapshot={number} kind={kind} grouped={group}'
            rows.append('PICA VS packet total ' + prefix + ' ' + text(counts(selected)))
            for bucket, (lo, hi) in enumerate(BOUNDS):
                values = [p for p in selected if lo <= p[2] <= hi]
                rows.append('PICA VS packet length ' + prefix + f' bucket={bucket} ' + text(counts(values)))
            masks = {'exclusive_packets': sum(p[6] for p in selected),
                     'mirrored_packets': sum(1-p[6] for p in selected)}
            masks.update({f'm{i}': sum(p[7] == i for p in selected) for i in range(16)})
            rows.append('PICA VS packet masks ' + prefix + ' ' + text(masks))
    rows.append(f'PICA VS packet end session={session} snapshot={number} '
                f'all_packets={len(cohort) if all_headers is None else all_headers}')
    assert len(rows) == 50
    return '\n'.join(rows) + '\n'


def log():
    return snapshot(256) + snapshot(512, COHORT, 17)


def replace_row(text, contains, transform):
    rows = text.splitlines()
    matches = [i for i, row in enumerate(rows) if contains in row]
    assert len(matches) == 1, (contains, matches)
    rows[matches[0]] = transform(rows[matches[0]])
    return '\n'.join(rows) + '\n'


class PacketSummaryTests(unittest.TestCase):
    def test_counts_units_matched_denominators_and_mirrors(self):
        report = packets.summarize(log())
        self.assertTrue(report['available'])
        self.assertEqual(report['all_parsed_header_packets'], 17)
        self.assertEqual(report['vs_source_packet_counts']['packets'], 9)
        self.assertEqual(report['vs_source_packet_counts']['extra_words'], 2577)
        self.assertEqual(report['vs_source_packet_counts']['eligible_packets'], 3)
        self.assertEqual(report['vs_source_packet_counts']['eligible_extra_words'], 9)
        ratios = report['frequencies']
        self.assertEqual(ratios['eligible_packets_of_vs_source_packets']['percent'], 100/3)
        self.assertEqual(ratios['eligible_packets_of_ungrouped_vs_source_packets']['denominator'], 7)
        self.assertEqual(ratios['eligible_packets_of_all_parsed_headers']['denominator'], 17)
        self.assertEqual(ratios['eligible_extra_words_of_declared_vs_extra_words']['denominator'], 2577)
        self.assertFalse(report['mirrored_internal_store_words']['available'])
        self.assertIsNone(report['mirrored_internal_store_words']['value'])
        self.assertIn('not CPU savings', report['limitations'][0])
        self.assertNotIn('cpu_timing', report)
        self.assertEqual(sum(c['mask_and_mirror_packet_counts']['mirrored_packets']
                             for c in report['classes']), 5)

    def test_zero_and_unavailable_are_different(self):
        report = packets.summarize(snapshot(256) + snapshot(1025))
        self.assertEqual(report['vs_source_packet_counts']['eligible_packets'], 0)
        self.assertIsNone(report['frequencies']['eligible_packets_of_vs_source_packets']['percent'])
        noneligible = [('program', 1, 3, 1, 1, 0, 1, 0)]
        report = packets.summarize(snapshot(256) + snapshot(512, noneligible))
        self.assertEqual(report['frequencies']['eligible_packets_of_vs_source_packets']['percent'], 0)
        for text in ('', 'PICA command options repeat_state=false command_profile=false\n', snapshot(256)):
            with self.subTest(text=text[:60]), self.assertRaisesRegex(ValueError, 'unavailable'):
                packets.summarize(text)

    def test_exact_selected_endpoints_and_no_cross_session_mix(self):
        data = log() + snapshot(768, COHORT*2, 34) + snapshot(256, session=99) + snapshot(512, session=99)
        with self.assertRaisesRegex(ValueError, 'multiple'):
            packets.summarize(data)
        report = packets.summarize(data, 41, (512, 768))
        self.assertEqual(report['all_parsed_header_packets'], 17)
        self.assertEqual([r['snapshot'] for r in report['endpoints']], [512, 768])
        self.assertEqual(packets.summarize(data, 99)['all_parsed_header_packets'], 0)
        for endpoints in ((256, 511), (512, 256), (256, 256)):
            with self.assertRaisesRegex(ValueError, 'exact snapshot endpoints'):
                packets.summarize(data, 41, endpoints)
        with self.assertRaisesRegex(ValueError, 'unavailable'):
            packets.summarize(data, 100)

    def test_every_missing_or_duplicated_snapshot_row_is_rejected(self):
        complete = snapshot(512, COHORT, 17).splitlines()
        for i in range(50):
            with self.subTest(missing=i), self.assertRaises(ValueError):
                packets.summarize(snapshot(256) + '\n'.join(complete[:i] + complete[i+1:]))
            with self.subTest(duplicate=i), self.assertRaises(ValueError):
                packets.summarize(snapshot(256) + '\n'.join(complete[:i] + [complete[i]] + complete[i:]))

    def test_all_bucket_boundaries(self):
        cohort = [('swizzle', 0, n, 1, 1, int(n >= 2), 0, n % 16)
                  for pair in BOUNDS for n in set(pair)]
        report = packets.summarize(snapshot(256) + snapshot(512, cohort))
        rows = report['classes'][2]['length_buckets']
        for bucket, bounds in enumerate(BOUNDS):
            self.assertEqual(rows[bucket]['packets'], len(set(bounds)))
            self.assertEqual(tuple(rows[bucket]['declared_extra_word_range']), bounds)

    def test_malformed_unknown_duplicate_and_overflow_fields(self):
        original = log()
        changes = [('schema=1', 'schema=2'), ('session=41', 'session=-1'),
                   ('session=41', 'session=18446744073709551616'),
                   ('schema=1', 'schema=1 schema=1'), ('schema=1', 'schema=1 unknown=0'),
                   ('kind=program', 'kind=geometry'), ('grouped=0', 'grouped=2'),
                   ('bucket=0', 'bucket=10'), ('packets=0', 'packets=+0')]
        for before, after in changes:
            with self.subTest(after=after), self.assertRaises(ValueError):
                packets.summarize(original.replace(before, after, 1))
        with self.assertRaises(ValueError):
            packets.summarize(original.replace('PICA VS packet length', 'PICA VS packet surprise', 1))

    def test_mixed_endpoints_footer_mismatch_and_resets(self):
        for marker in ('total', 'length', 'masks', 'end'):
            needle = f'PICA VS packet {marker} session=41 snapshot=512'
            for field, value in [('session=41', 'session=42'), ('snapshot=512', 'snapshot=513')]:
                with self.subTest(marker=marker, field=field), self.assertRaises(ValueError):
                    packets.summarize(log().replace(needle, needle.replace(field, value), 1))
        with self.assertRaisesRegex(ValueError, 'footer'):
            packets.summarize(log().replace('end session=41 snapshot=512 all_packets=17',
                                           'end session=41 snapshot=512 all_packets=18'))
        for number in (256, 255):
            with self.assertRaisesRegex(ValueError, 'advance'):
                packets.summarize(snapshot(256) + snapshot(number))
        with self.assertRaisesRegex(ValueError, 'decreased'):
            packets.summarize(snapshot(256, COHORT, 17) + snapshot(512))
        # Same all-header count cannot hide a reset in a specific bucket or mask.
        with self.assertRaisesRegex(ValueError, 'decreased'):
            packets.summarize(snapshot(256, COHORT, 17) + snapshot(512, (), 18))

    def test_count_bucket_mask_and_interval_invariants(self):
        data = log()
        cases = [
            ('total session=41 snapshot=512 kind=program grouped=0 ', 'packets=5', 'packets=4'),
            ('length session=41 snapshot=512 kind=program grouped=0 bucket=1 ', 'extra_words=1', 'extra_words=2'),
            ('length session=41 snapshot=512 kind=program grouped=0 bucket=2 ', 'eligible_extra_words=5', 'eligible_extra_words=7'),
            ('length session=41 snapshot=512 kind=program grouped=1 bucket=4 ', 'eligible_packets=0', 'eligible_packets=1'),
            ('masks session=41 snapshot=512 kind=program grouped=0 ', 'm15=2', 'm15=1'),
            ('masks session=41 snapshot=512 kind=program grouped=0 ', 'mirrored_packets=3', 'mirrored_packets=2'),
        ]
        for needle, before, after in cases:
            with self.subTest(needle=needle), self.assertRaises(ValueError):
                packets.summarize(replace_row(data, needle, lambda row: row.replace(before, after)))
        with self.assertRaisesRegex(ValueError, 'exceed all'):
            packets.summarize(snapshot(256) + snapshot(512, COHORT, 8))
        # A valid interval adds one eligible packet with matching cohort counts.
        before = [('program', 0, 2, 1, 1, 1, 1, 1),
                  ('program', 0, 2, 0, 0, 0, 1, 1)]
        after = [('program', 0, 2, 1, 1, 1, 1, 1),
                 ('program', 0, 2, 0, 0, 0, 1, 1),
                 ('program', 0, 2, 1, 1, 1, 1, 1)]
        valid = packets.summarize(snapshot(256, before) + snapshot(512, after))
        self.assertEqual(valid['vs_source_packet_counts']['eligible_packets'], 1)
        disjoint = [('program', 0, 2, 1, 0, 0, 1, 1),
                    ('program', 0, 2, 0, 1, 0, 1, 1)]
        overlapping = [('program', 0, 2, 1, 1, 1, 1, 1),
                       ('program', 0, 2, 0, 0, 0, 1, 1),
                       ('program', 0, 2, 0, 0, 0, 1, 1)]
        # Both endpoints independently satisfy their cohort bounds and every
        # counter is nondecreasing, yet the delta's eligibility lacks support.
        with self.assertRaisesRegex(ValueError, 'interval: complete/capacity/eligible'):
            packets.summarize(snapshot(256, disjoint) + snapshot(512, overlapping))

    def test_legacy_summary_behavior_unchanged_and_new_rows_ignored(self):
        options = 'PICA command options repeat_state=false command_profile=true\n'
        def old(n):
            return 'PICA command profile ' + ' '.join(f'{k}={n if k in ("lists", "writes", "list_cpu_ns") else 0}' for k in legacy.FIELDS) + '\n'
        data = options + old(1) + old(2)
        self.assertEqual(legacy.summarize(data), legacy.summarize(data + log()))
        with self.assertRaises(ValueError):
            legacy.summarize(data.replace('command_profile=true', 'command_profile=true packet_profile=1'))
        with self.assertRaises(ValueError):
            legacy.summarize(data.replace('writes=1', 'writes=1 packet_count=0'))

    def test_cli_json_and_fail_closed(self):
        with tempfile.TemporaryDirectory() as area:
            p = Path(area) / 'log.txt'
            p.write_text(log())
            result = subprocess.run([sys.executable, str(ROOT/'tools/summarize_shader_packets.py'), str(p)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('"available": true', result.stdout)
            p.write_text(snapshot(256))
            result = subprocess.run([sys.executable, str(ROOT/'tools/summarize_shader_packets.py'), str(p)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, '')
            self.assertIn('unavailable', result.stderr)


if __name__ == '__main__':
    unittest.main()
