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


def queue_fixture():
    rows = []
    def add(kind, sid=0, **fields):
        row = dict.fromkeys(TRACE.FIELDS, 0)
        row.update(sequence=len(rows), frame=0, submission=sid,
                   timer0_ticks=100, current_ticks=100, global_ticks=100,
                   kind=TRACE.KINDS.index(kind), thread=11, object=2)
        row.update(fields);rows.append(row)
    def snapshot(phase, header, slot=0xffffffff, word=0, sid=0):
        add('QueueState',sid,a=phase,b=header,c=slot,d=word)
    add('VBlank')
    snapshot(0,0xfedc020e)
    add('GXBegin',1,a=1,b=0xfedc020e,c=2)
    snapshot(1,0xfedc020e,0,0x80ff0001,1)
    add('GXEnd',1,c=1,d=0xfedc0100)
    snapshot(2,0xfedc0100,0,0x81ff0001,1)
    add('GXBegin',2,a=5,b=0xfedc0100,c=(1<<32)|2)
    snapshot(1,0xfedc0100,1,0xbbccdd05,2)
    add('GXEnd',2,c=1,d=0xfedc0101)
    snapshot(2,0xfedc0101,1,0xaaccdd05,2)
    snapshot(3,0xfedc0101,sid=2)
    snapshot(4,0xfedc0001,sid=2)
    return dict(schema_version=2,start_frame=0,duration_vblanks=1,records=len(rows),
                complete=1,truncated=0,foreign_events_dropped=0),rows


def csv_fixture(metadata,rows,version):
    return '# foreign_events_dropped=0\n# cara_guest_gpu_trace_v'+str(version)+','+','.join(
        f'{k}={v}' for k,v in metadata.items() if k not in ('schema_version','foreign_events_dropped'))+'\n'+','.join(
        TRACE.FIELDS)+'\n'+'\n'.join(','.join(str(r[k]) for k in TRACE.FIELDS) for r in rows)+'\n'


class QueueStateV2Tests(unittest.TestCase):
    def test_raw_header_command_words_multi_command_wrap_and_changed_high_bits(self):
        result=TRACE.analyze(*TRACE.read_trace(csv_fixture(*queue_fixture(),2)))
        queue=result['queue_state']
        self.assertEqual(queue['completed_batches'],1)
        self.assertEqual(queue['command_phase_pairs'],2)
        first,second=queue['commands']
        self.assertEqual(first['before']['raw_header'],0xfedc020e)
        self.assertEqual(first['after']['raw_header'],0xfedc0100)
        self.assertEqual(first['before']['raw_command_word'],0x80ff0001)
        self.assertEqual(first['after']['raw_command_word'],0x81ff0001)
        self.assertEqual(first['raw_command_word_changed_bits'],0x01000000)
        self.assertEqual(second['chosen_slot'],1)
        self.assertEqual(second['caller_thread'],11)
        self.assertEqual(second['gsp_recipient'],2)
        self.assertEqual(queue['batches'][0]['raw_clear_changed_bits'],0x100)
        self.assertIn('not labeled busy',queue['limitation'])
        self.assertNotIn('busy',first['before'])

    def test_version1_rejects_new_kind_unknown_versions_and_reserved_version_metadata(self):
        metadata,rows=queue_fixture()
        with self.assertRaises(ValueError):
            TRACE.analyze(*TRACE.read_trace(csv_fixture(metadata,rows,1)))
        for version in (0,3,99):
            with self.assertRaisesRegex(ValueError,'version'):
                TRACE.read_trace(csv_fixture(metadata,rows,version))
        text=csv_fixture(metadata,rows,2)
        for bad in (text.replace('start_frame=0','schema_version=1,start_frame=0'),
                    '# cara_guest_gpu_trace_v1,records=0\n'+text):
            with self.assertRaises(ValueError):TRACE.read_trace(bad)

    def test_missing_command_or_batch_phase_rejected_and_partial_explicit(self):
        for removed in (1,3,5,10,11):
            metadata,rows=queue_fixture();del rows[removed]
            for i,r in enumerate(rows):r['sequence']=i
            metadata['records']=len(rows)
            with self.assertRaises(ValueError):TRACE.analyze(metadata,rows)
        metadata,rows=queue_fixture();rows.pop();metadata['records']=len(rows)
        result=TRACE.analyze(metadata,rows,allow_partial=True)
        self.assertFalse(result['capture']['usable_as_complete_capture'])
        self.assertTrue(result['queue_state']['incomplete_observations'])

    def test_removing_both_command_snapshots_is_not_silently_accepted(self):
        metadata,rows=queue_fixture();rows=[row for i,row in enumerate(rows) if i not in (3,5)]
        for i,row in enumerate(rows):row['sequence']=i
        metadata['records']=len(rows)
        with self.assertRaisesRegex(ValueError,'missing QueueState'):
            TRACE.analyze(metadata,rows)

    def test_phase_identity_adjacent_timestamp_raw_width_and_slot_boundaries(self):
        changes=[(3,dict(a=5)),(3,dict(b=1<<32)),(3,dict(d=1<<32)),
                 (3,dict(c=15)),(5,dict(submission=99)),(5,dict(c=1)),
                 (3,dict(current_ticks=101,timer0_ticks=101)),(10,dict(c=0)),
                 (10,dict(d=1)),(5,dict(b=5))]
        for index,fields in changes:
            metadata,rows=queue_fixture();rows[index].update(fields)
            with self.assertRaises(ValueError):TRACE.analyze(metadata,rows)
        metadata,rows=queue_fixture()
        rows[6]['c']=(14<<32)|2;rows[7]['c']=rows[9]['c']=14
        self.assertEqual(TRACE.analyze(metadata,rows)['queue_state']['commands'][1]['chosen_slot'],14)

    def test_empty_batch_retains_raw_header_and_absent_command_sentinel(self):
        metadata,rows=queue_fixture();rows=[rows[i] for i in (0,1,10,11)]
        for i,r in enumerate(rows):r['sequence']=i
        metadata['records']=len(rows)
        queue=TRACE.analyze(metadata,rows)['queue_state']
        self.assertEqual(queue['completed_batches'],1)
        self.assertEqual(queue['command_phase_pairs'],0)
        self.assertEqual(queue['batches'][0]['entry']['chosen_slot'],0xffffffff)
        self.assertFalse(queue['batches'][0]['entry']['command_word_present'])


if __name__ == '__main__':
    unittest.main()
