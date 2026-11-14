"""Synthetic ordering/volume fixtures; these do not model hardware GPU timing."""
import copy
import importlib.util
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    'guest_gpu_trace', Path(__file__).resolve().parents[1] / 'tools/analyze_guest_gpu_trace.py')
TRACE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TRACE)


def fixture(first_draws=10):
    rows = []
    sid = 0
    def add(kind, frame, **values):
        row = dict.fromkeys(TRACE.FIELDS, 0)
        row.update(sequence=len(rows), frame=frame, submission=sid, kind=TRACE.KINDS.index(kind),
                   timer0_ticks=1000 * frame + 100, current_ticks=1000 * frame + 100,
                   global_ticks=1000 * frame + 100)
        row.update(values)
        rows.append(row)
    for frame in range(16):
        add('VBlank', frame)
        add('Swap', frame, a=0)
        add('WaitBlock', frame, thread=11, object=50)
        sid += 1
        add('GXBegin', frame, thread=22, a=1, b=256, d=(0x1000 << 32) | 64)
        add('IRQRequest', frame, object=50, a=5)
        add('Relay', frame, object=50, a=5, d=1)
        add('EventSignal', frame, object=50)
        add('EventAcquire', frame, thread=11, object=50)
        add('Wake', frame, thread=11, object=50)
        add('GXEnd', frame, thread=22, a=first_draws, b=100, c=1)
        add('Run', frame, thread=11)
        sid += 1
        add('GXBegin', frame, thread=11, a=5, b=256)
        add('GXEnd', frame, thread=11, c=1)
        sid += 1
        add('GXBegin', frame, thread=11, a=1, b=256, d=(0x2000 << 32) | 96)
        add('GXEnd', frame, thread=11, a=20, b=200, c=1)
    return dict(start_frame=0, duration_vblanks=16, records=len(rows), complete=1,
                truncated=0, foreign_events_dropped=0), rows


class GuestGPUTraceTests(unittest.TestCase):
    def test_volume_changes_without_swap_or_list_frequency_change(self):
        light = TRACE.analyze(*fixture())
        heavy = TRACE.analyze(*fixture(100))
        for result in (light, heavy):
            self.assertTrue(result['capture']['usable_as_complete_capture'])
            self.assertEqual(len(result['per_list']), 32)
            self.assertEqual(sum(row['top_swaps'] for row in result['per_vblank']), 16)
            self.assertEqual(result['irq_delivery']['same_timer0_tick'], 16)
            self.assertEqual(len(result['between_consecutive_top_swaps']), 15)
        self.assertEqual(sum(row['draws'] for row in light['per_vblank']), 480)
        self.assertEqual(sum(row['draws'] for row in heavy['per_vblank']), 1920)
        self.assertEqual(light['between_consecutive_top_swaps'][0]['draws'], 30)
        self.assertEqual(heavy['between_consecutive_top_swaps'][0]['draws'], 120)

    def test_ready_run_next_gx_and_later_list_are_distinct_observations(self):
        result = TRACE.analyze(*fixture())
        self.assertEqual(result['direct_completion_to_producer']['observed_chains'], 16)
        chain = result['direct_completion_to_producer']['examples'][0]
        self.assertTrue(chain['wait_block_observed'])
        self.assertTrue(chain['run_observed'])
        self.assertEqual(chain['activity'], 'Wake')
        self.assertEqual(chain['next_command_id'], 5)  # First successor is cache flush.
        self.assertEqual(chain['subsequent_list_submission'], 3)
        self.assertEqual(chain['subsequent_list_draws'], 20)

    def test_intervening_other_event_acquire_does_not_reuse_old_completion(self):
        metadata, rows = fixture()
        for row in rows:
            if row['kind'] == TRACE.KINDS.index('Run'):
                row.update(kind=TRACE.KINDS.index('EventAcquire'), object=99)
        result = TRACE.analyze(metadata, rows)
        self.assertEqual(result['direct_completion_to_producer']['observed_chains'], 0)

    def test_incomplete_truncated_and_foreign_observations_rejected_by_default(self):
        for key, value in [('complete', 0), ('truncated', 1), ('foreign_events_dropped', 1)]:
            metadata, rows = fixture()
            metadata[key] = value
            with self.assertRaises(ValueError):
                TRACE.analyze(metadata, rows)
            result = TRACE.analyze(metadata, rows, allow_partial=True)
            self.assertFalse(result['capture']['usable_as_complete_capture'])
            self.assertTrue(result['capture']['limitations'])

    def test_current_ticks_are_compared_only_with_same_domain(self):
        metadata, rows = fixture()
        rows[1].update(core=1, current_ticks=1)
        result = TRACE.analyze(metadata, rows)
        self.assertEqual(result['capture']['current_timer_domains'], [0, 1])
        rows[2].update(core=1, current_ticks=0)
        with self.assertRaisesRegex(ValueError, 'domain regressed'):
            TRACE.analyze(metadata, rows)

    def test_malformed_sequence_boundaries_and_counter_availability(self):
        metadata, rows = fixture()
        for mutate in (lambda data: data[1].update(sequence=999),
                       lambda data: data[0].update(kind=1),
                       lambda data: data[-1].update(c=2)):
            changed = copy.deepcopy(rows)
            mutate(changed)
            with self.assertRaises(ValueError):
                TRACE.analyze(metadata, changed)
        for row in rows:
            if row['kind'] == TRACE.KINDS.index('GXEnd'):
                row['c'] = 0
        result = TRACE.analyze(metadata, rows)
        self.assertIsNone(result['per_vblank'][0]['draws'])
        self.assertIsNone(result['per_list'][0]['gpu_vertices'])

    def test_four_interval_draw_triplets_partition_captured_state_conservatively(self):
        metadata, original = fixture()
        metadata.update(duration_vblanks=4, draws_enabled=1)
        rows = []
        for row in original:
            if row['frame'] >= 4:
                continue
            if row['kind'] == TRACE.KINDS.index('GXEnd') and row['thread'] == 22:
                draw = dict(row, kind=17, a=(0x1000 << 32) | 0x2000,
                            b=100 | (1 << 32), c=0, d=0)
                state = dict(row, kind=18, a=400 | (240 << 16) | (1 << 36),
                             b=17, c=0, d=0)
                inputs = dict(row, kind=19, a=(0x3000 << 32) | 16,
                              b=1 << 32, c=0x4000 << 32, d=0)
                rows.extend((draw, state, inputs))
            rows.append(row)
        for index, row in enumerate(rows):
            row['sequence'] = index
        metadata['records'] = len(rows)
        result = TRACE.analyze(metadata, rows)
        summary = result['draw_passes']
        self.assertEqual(summary['captured_draws'], 4)
        self.assertEqual(summary['trigger_vertices'], 400)
        self.assertEqual(summary['framebuffer_partitions'][0]['draws'], 4)
        self.assertEqual(summary['captured_state_groups'], 1)
        self.assertEqual(summary['largest_captured_state_groups'][0]['vertex_base'], 0x3000)
        self.assertIn('never duplicate proof', summary['limitation'])
        broken = copy.deepcopy(rows)
        next(row for row in broken if row['kind'] == 19)['kind'] = 8
        with self.assertRaisesRegex(ValueError, 'draw triplet'):
            TRACE.analyze(metadata, broken)

    def test_new_hid_and_sleep_kinds_are_accepted_without_changing_old_analysis(self):
        metadata, rows = fixture()
        rows[1].update(kind=20, object=50, a=51, b=1, c=(1 << 64) - 1)
        rows[2].update(kind=21, thread=11, a=1000)
        result = TRACE.analyze(metadata, rows)
        self.assertEqual(result['hid_updates'], 1)
        self.assertEqual(result['sleep_requests'], 1)

    def test_csv_and_metadata_are_validated_and_ring_fields_are_separate(self):
        metadata, rows = fixture()
        rows[3].update(b=3 | (2 << 8), c=(1 << 32) | 2)
        text = '# foreign_events_dropped=0\n# cara_guest_gpu_trace_v1,'
        text += ','.join(f'{key}={value}' for key, value in metadata.items()
                         if key != 'foreign_events_dropped') + '\n'
        text += ','.join(TRACE.FIELDS) + '\n'
        text += '\n'.join(','.join(str(row[key]) for key in TRACE.FIELDS) for row in rows)
        parsed_metadata, parsed_rows = TRACE.read_trace(text)
        result = TRACE.analyze(parsed_metadata, parsed_rows)
        first = result['per_list'][0]
        self.assertEqual((first['ring_index'], first['ring_count'], first['chosen_slot'],
                          first['active_recipient']), (3, 2, 1, 2))
        with self.assertRaises(ValueError):
            TRACE.read_trace(text.replace('records=240', 'records=bad'))


if __name__ == '__main__':
    unittest.main()
