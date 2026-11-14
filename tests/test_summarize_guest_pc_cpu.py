import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'summary', Path(__file__).parents[1] / 'tools/summarize_guest_pc_cpu.py')
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


def fixture():
    return {'frame': 120, 'cores': [{'core': 0, 'guest_pc_cpu': {
        'enabled': True, 'config_error': '', 'kind': summary.KIND,
        'base_frame': 100, 'frame_resets': 0,
        'ranges': [{'name': 'body', 'start': 0x1000, 'end': 0x1100},
                   {'name': 'helper', 'start': 0x2000, 'end': 0x2100}],
        'window_frames': 2, 'period_frames': 10, 'window_count': 1, 'start_frame': 0,
        'sampled_frame_ids': [100, 101], 'sampled_refreshes': 2,
        'windows': [{'index': 0, 'start_frame': 100, 'end_frame': 102,
                     'observed_frames': 2, 'closed': True, 'complete': True}],
        'cpu_ns': 1000, 'clock_reads': 100, 'clock_failures': 0,
        'unassigned_intervals': 0, 'coverage_complete': True,
        'calibration_reads': 64, 'calibration_failures': 0, 'clock_ns_per_read': 1.5,
        'sample_capacity_exhausted': False,
        'thread_table_overflow_entries': 0, 'straddling_blocks': 0,
        'threads': [{'thread_id': 3, 'overflow': False, 'cpu_ns': 1000,
                     'buckets': [
                         {'name': 'body', 'cpu_ns': 600, 'blocks': 50, 'transitions': 20},
                         {'name': 'helper', 'cpu_ns': 300, 'blocks': 30, 'transitions': 20},
                         {'name': 'other_guest', 'cpu_ns': 80, 'blocks': 3, 'transitions': 2},
                         {'name': 'svc_external', 'cpu_ns': 20, 'blocks': 0, 'transitions': 2}]}]
    }}]}


def episode_fixture():
    report = fixture()
    profile = report['cores'][0]['guest_pc_cpu']
    profile.update(mode='episode', episode_start_pc=0x1000, episode_end_pc=0x1100)
    thread = profile['threads'][0]
    thread['buckets'] = [
        {'name': 'episode', 'cpu_ns': 900, 'blocks': 80, 'transitions': 4},
        {'name': 'other_guest', 'cpu_ns': 80, 'blocks': 3, 'transitions': 2},
        {'name': 'svc_external', 'cpu_ns': 20, 'blocks': 0, 'transitions': 2}]
    thread['episode'] = {
        'started': 10, 'completed': 9, 'aborted': 0, 'reentries': 500,
        'sampled_completed_cpu_ns': 600, 'sampled_partial_cpu_ns': 200,
        'sampled_aborted_cpu_ns': 0, 'active_sampled_cpu_ns': 100,
        'partial_sampled_episodes': 2, 'sampled_aborted_episodes': 0,
        'active': True, 'active_partial': True}
    return report


class GuestPCSummaryTests(unittest.TestCase):
    def test_episode_spans_remain_distinct_from_completed_episode_cpu(self):
        result = summary.summarize(episode_fixture())['cores'][0]
        self.assertEqual(result['mode'], 'episode')
        self.assertEqual(result['status'], 'complete')
        self.assertEqual({b['name'] for b in result['buckets']},
                         {'other_guest', 'svc_external', 'episode'})
        self.assertEqual(result['episode']['sampled_span_cpu_ns'], 900)
        self.assertEqual(result['episode']['sampled_completed_cpu_ns'], 600)
        self.assertEqual(result['episode']['cpu_ns_per_sampled_refresh']
                         ['sampled_span_cpu_ns'], 450)
        self.assertFalse(result['threads'][0]['episode']
                         ['all_sampled_cpu_from_fully_completed_episodes'])
        self.assertNotIn('cpu_ns_per_completed_episode', result['episode'])

    def test_sampled_abort_invalidates_normalization_unsampled_abort_does_not(self):
        report = episode_fixture()
        episode = report['cores'][0]['guest_pc_cpu']['threads'][0]['episode']
        episode['aborted'] = 1
        self.assertEqual(summary.summarize(report)['cores'][0]['status'], 'complete')
        episode['sampled_aborted_episodes'] = 1
        episode['sampled_aborted_cpu_ns'] = 50
        episode['sampled_completed_cpu_ns'] -= 50
        core = summary.summarize(report)['cores'][0]
        self.assertEqual(core['status'], 'incomplete')
        self.assertIsNone(core['episode']['cpu_ns_per_sampled_refresh']['sampled_span_cpu_ns'])
        self.assertEqual(core['episode']['sampled_aborted_cpu_ns'], 50)

    def test_zero_cpu_sampled_abort_still_invalidates_episode_result(self):
        report = episode_fixture()
        episode = report['cores'][0]['guest_pc_cpu']['threads'][0]['episode']
        episode.update(aborted=1, sampled_aborted_episodes=1)
        core = summary.summarize(report)['cores'][0]
        self.assertEqual(core['episode']['sampled_aborted_cpu_ns'], 0)
        self.assertFalse(core['valid_for_per_refresh'])

    def test_partial_window_suppresses_episode_span_normalization(self):
        report = episode_fixture()
        profile = report['cores'][0]['guest_pc_cpu']
        profile.update(sampled_frame_ids=[100], sampled_refreshes=1, coverage_complete=False)
        profile['windows'][0].update(observed_frames=1, closed=False, complete=False)
        core = summary.summarize(report)['cores'][0]
        self.assertEqual(core['episode']['sampled_span_cpu_ns'], 900)
        self.assertIsNone(core['episode']['cpu_ns_per_sampled_refresh']['sampled_span_cpu_ns'])

    def test_episode_subtotals_and_active_state_are_validated(self):
        for failure in ('sum', 'inactive', 'bad_fence', 'missing_stats', 'unknown_mode'):
            with self.subTest(failure=failure):
                report = episode_fixture()
                profile = report['cores'][0]['guest_pc_cpu']
                episode = profile['threads'][0]['episode']
                if failure == 'sum': episode['sampled_completed_cpu_ns'] += 1
                elif failure == 'inactive': episode['active'] = False
                elif failure == 'bad_fence': profile['episode_start_pc'] = 0x3000
                elif failure == 'missing_stats': del episode['sampled_partial_cpu_ns']
                elif failure == 'unknown_mode': profile['mode'] = 'native'
                with self.assertRaises((ValueError, KeyError)):
                    summary.summarize(report)

    def test_reconciled_raw_time_and_overhead_never_subtracted(self):
        result = summary.summarize(fixture())
        core = result['cores'][0]
        self.assertEqual(core['status'], 'complete')
        self.assertEqual(core['cpu_ns'], 1000)
        self.assertEqual(core['cpu_ns_per_sampled_refresh'], 500)
        self.assertEqual(core['clock_read_budget']['estimated_cpu_ns'], 150)
        self.assertFalse(core['clock_read_budget']['subtracted_from_cpu_ns'])
        self.assertEqual(result['aggregate']['cpu_ns_per_sampled_refresh'], 500)
        self.assertNotIn('loop_inclusive_cpu_ns', core)

    def test_cores_share_frame_denominator_not_sum(self):
        report = fixture()
        second = copy.deepcopy(report['cores'][0])
        second['core'] = 1
        report['cores'].append(second)
        result = summary.summarize(report)['aggregate']
        self.assertEqual(result['cpu_ns'], 2000)
        self.assertEqual(result['sampled_refreshes'], 2)
        self.assertEqual(result['cpu_ns_per_sampled_refresh'], 1000)
        second['guest_pc_cpu']['sampled_frame_ids'] = [110, 111]
        second['guest_pc_cpu']['base_frame'] = 110
        window = second['guest_pc_cpu']['windows'][0]
        window.update(start_frame=110, end_frame=112)
        result = summary.summarize(report)['aggregate']
        self.assertFalse(result['same_complete_sampled_frames'])
        self.assertIsNone(result['cpu_ns_per_sampled_refresh'])

    def test_clock_failures_keep_raw_time_but_disable_normalization(self):
        report = fixture()
        profile = report['cores'][0]['guest_pc_cpu']
        profile.update(clock_failures=1, coverage_complete=False)
        core = summary.summarize(report)['cores'][0]
        self.assertEqual(core['cpu_ns'], 1000)
        self.assertEqual(core['status'], 'incomplete')
        self.assertIsNone(core['cpu_ns_per_sampled_refresh'])
        self.assertIsNone(core['clock_read_budget']['estimated_cpu_ns_per_sampled_refresh'])

    def test_partial_window_has_no_per_refresh_claim(self):
        report = fixture()
        profile = report['cores'][0]['guest_pc_cpu']
        profile.update(sampled_frame_ids=[100], sampled_refreshes=1, coverage_complete=False)
        profile['windows'][0].update(observed_frames=1, closed=False, complete=False)
        core = summary.summarize(report)['cores'][0]
        self.assertIsNone(core['cpu_ns_per_sampled_refresh'])
        self.assertIn('window 0 incomplete or open', core['issues'])

    def test_future_requested_windows_suppress_normalization(self):
        report = fixture()
        report['cores'][0]['guest_pc_cpu']['window_count'] = 3
        core = summary.summarize(report)['cores'][0]
        self.assertEqual(core['status'], 'incomplete')
        self.assertEqual(core['requested_windows_not_started'], 2)
        self.assertIsNone(core['cpu_ns_per_sampled_refresh'])

    def test_reliability_failures_even_if_coverage_claims_true(self):
        for field in ('frame_resets', 'calibration_failures', 'unassigned_intervals',
                      'straddling_blocks', 'thread_table_overflow_entries'):
            with self.subTest(field=field):
                report = fixture()
                report['cores'][0]['guest_pc_cpu'][field] = 1
                core = summary.summarize(report)['cores'][0]
                self.assertFalse(core['valid_for_per_refresh'])
        report = fixture()
        report['cores'][0]['guest_pc_cpu']['sample_capacity_exhausted'] = True
        self.assertFalse(summary.summarize(report)['cores'][0]['valid_for_per_refresh'])

    def test_disabled_due_to_bad_config_keeps_error_visible(self):
        report = {'cores': [{'core': 0, 'guest_pc_cpu': {
            'enabled': False, 'config_error': 'overlapping ranges'}}]}
        result = summary.summarize(report)
        self.assertEqual(result['cores'][0]['status'], 'invalid_config')
        self.assertIn('overlapping ranges', result['cores'][0]['issues'][0])

    def test_missing_old_and_disabled_reports_are_unavailable_not_zero(self):
        for core in ({'core': 0}, {'core': 0, 'guest_pc_cpu': {'enabled': False}}):
            result = summary.summarize({'cores': [core]})
            self.assertIsNone(result['aggregate']['cpu_ns'])
            self.assertIsNone(result['aggregate']['cpu_ns_per_sampled_refresh'])

    def test_no_calibration_is_unavailable_not_free_clock(self):
        report = fixture()
        report['cores'][0]['guest_pc_cpu'].update(calibration_reads=0, clock_ns_per_read=0)
        budget = summary.summarize(report)['cores'][0]['clock_read_budget']
        self.assertFalse(budget['calibration_available'])
        self.assertIsNone(budget['estimated_cpu_ns'])

    def test_malformed_reports_rejected(self):
        def mutate(report, name):
            profile = report['cores'][0]['guest_pc_cpu']
            if name == 'core_total': profile['cpu_ns'] += 1
            elif name == 'thread_total': profile['threads'][0]['cpu_ns'] += 1
            elif name == 'missing_bucket': profile['threads'][0]['buckets'].pop()
            elif name == 'overlap': profile['ranges'][1]['start'] = 0x1080
            elif name == 'duplicate_frame': profile['sampled_frame_ids'][1] = 100
            elif name == 'outside_window': profile['sampled_frame_ids'][1] = 105
            elif name == 'negative_time': profile['cpu_ns'] = -1
            elif name == 'boolean_time': profile['cpu_ns'] = True
            elif name == 'bad_kind': profile['kind'] = 'time_samples'
            elif name == 'nan_clock': profile['clock_ns_per_read'] = float('nan')
        for name in ('core_total', 'thread_total', 'missing_bucket', 'overlap',
                     'duplicate_frame', 'outside_window', 'negative_time',
                     'boolean_time', 'bad_kind', 'nan_clock'):
            with self.subTest(name=name):
                report = fixture()
                mutate(report, name)
                with self.assertRaises(ValueError):
                    summary.summarize(report)


if __name__ == '__main__':
    unittest.main()
