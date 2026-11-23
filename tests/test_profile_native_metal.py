"""Profiler evidence preparation without launching an emulator or GPU workload."""
import importlib.util
import io
import json
import errno
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
SPEC=importlib.util.spec_from_file_location('profile_native_metal',ROOT/'tools/profile_native_metal.py')
profile=importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(profile)


class ProfileEvidenceTests(unittest.TestCase):
    def test_checkpoint_modes_cannot_silently_reset_measured_history(self):
        stderr=io.StringIO()
        argv=['profile_native_metal.py','--rom','unused.3ds','--output','unused',
              '--save-state-at','3000','--save-state-at-end']
        with mock.patch('sys.argv',argv),mock.patch('sys.stderr',stderr):
            with self.assertRaises(SystemExit) as error:profile.main()
        self.assertEqual(error.exception.code,2)
        self.assertIn('not allowed with argument',stderr.getvalue())

    def test_compiler_training_is_binary_bound_and_output_isolated(self):
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);env={'LLVM_PROFILE_FILE':'outside.profraw'}
            self.assertFalse(profile.configure_compiler_profile(output,{'status':'missing'},env))
            for mode in ('off','use','generate'):
                (output/'native-metal-audit.json').write_text(json.dumps(
                    {'compiler_policy':{'fastinterp_pgo':{'mode':mode}}}))
                actual=profile.configure_compiler_profile(output,{'status':'binary_matched'},env)
                self.assertEqual(actual,mode=='generate')
                self.assertEqual(env['LLVM_PROFILE_FILE'],str(output/'fastinterp-%p.profraw')
                                 if actual else 'outside.profraw')

    def test_jit_requires_explicit_backend_and_binary_matched_capability_audit(self):
        with tempfile.TemporaryDirectory() as folder:
            build=Path(folder)
            profile.validate_cpu_backend(build,'actual','fastinterp')
            with self.assertRaises(ValueError):profile.validate_cpu_backend(build,'actual','jit')
            audit=build/'native-metal-audit.json'
            for data in ({'binary_sha256':'actual','cpu_jit_enabled':False},
                         {'binary_sha256':'different','cpu_jit_enabled':True}):
                audit.write_text(json.dumps(data))
                with self.assertRaises(ValueError):profile.validate_cpu_backend(build,'actual','jit')
            audit.write_text(json.dumps({'binary_sha256':'actual','cpu_jit_enabled':True}))
            profile.validate_cpu_backend(build,'actual','jit')
            with self.assertRaises(ValueError):profile.validate_cpu_backend(build,'actual','unknown')

    def test_fresh_input_copy_has_exact_bytes_metadata_and_independent_writes(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'source';copied=root/'copied'
            source.write_bytes(bytes(range(256))*1024)
            source.chmod(0o640);os.utime(source,ns=(1600000000000000000,1600000000123456000))
            before=source.stat();expected=profile.file_sha256(source)
            profile.copy_input_file(source,copied)
            self.assertEqual(profile.file_sha256(copied),expected)
            self.assertNotEqual((source.stat().st_dev,source.stat().st_ino),(copied.stat().st_dev,copied.stat().st_ino))
            self.assertEqual(stat.S_IMODE(copied.stat().st_mode),0o640)
            self.assertEqual(copied.stat().st_mtime_ns,before.st_mtime_ns)
            copied.write_bytes(b'isolated destination')
            self.assertEqual(profile.file_sha256(source),expected)
            self.assertEqual(source.stat().st_mtime_ns,before.st_mtime_ns)

    def test_partial_clone_failure_falls_back_without_leaking_temporary_data(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'source';copied=root/'copied'
            source.write_bytes(b'complete input')
            source.chmod(0o640);before=source.stat()
            def unsupported(source_path,temporary):
                temporary.write_bytes(b'partial clone')
                raise OSError(errno.ENOTSUP,'Cloning unsupported')
            with mock.patch.object(profile,'_clone_input_file',side_effect=unsupported):
                self.assertFalse(profile.copy_input_file(source,copied))
            self.assertEqual(copied.read_bytes(),b'complete input')
            self.assertEqual(stat.S_IMODE(copied.stat().st_mode),0o640)
            self.assertEqual(copied.stat().st_mtime_ns,before.st_mtime_ns)
            copied.write_bytes(b'changed')
            self.assertEqual(source.read_bytes(),b'complete input')
            self.assertEqual(sorted(p.name for p in root.iterdir()),['copied','source'])

    def test_failed_fallback_removes_partial_copy_and_preserves_existing_inputs(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'source';destination=root/'destination'
            source.write_bytes(b'unchanged source')
            def fails(source_path,temporary,**kwargs):
                temporary.write_bytes(b'partial ordinary copy')
                raise OSError(errno.ENOSPC,'No space')
            with mock.patch.object(profile,'_clone_input_file',return_value=False),mock.patch.object(profile.shutil,'copy2',side_effect=fails):
                with self.assertRaises(OSError):profile.copy_input_file(source,destination)
            self.assertEqual(source.read_bytes(),b'unchanged source')
            self.assertEqual(list(root.iterdir()),[source])

    def test_input_copy_never_overwrites_existing_file_or_follows_links(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'source';destination=root/'destination';link=root/'link'
            source.write_bytes(b'input');destination.write_bytes(b'prior evidence')
            with self.assertRaises(FileExistsError):profile.copy_input_file(source,destination)
            self.assertEqual(destination.read_bytes(),b'prior evidence')
            link.symlink_to(source)
            with self.assertRaises(ValueError):profile.copy_input_file(link,root/'fresh')
            with self.assertRaises(FileExistsError):profile.copy_input_file(source,link)
            self.assertEqual(source.read_bytes(),b'input')

    def test_input_copy_publication_failure_cleans_only_owned_partial_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'source';destination=root/'destination'
            source.write_bytes(b'input')
            with mock.patch.object(profile.os,'replace',side_effect=PermissionError('Publication denied')):
                with self.assertRaises(PermissionError):profile.copy_input_file(source,destination)
            self.assertEqual(list(root.iterdir()),[source])
            original_copy=profile.shutil.copy2
            def raced(source_path,temporary,**kwargs):
                result=original_copy(source_path,temporary,**kwargs)
                destination.write_bytes(b'concurrent evidence')
                return result
            with mock.patch.object(profile,'_clone_input_file',return_value=False),mock.patch.object(profile.shutil,'copy2',side_effect=raced):
                with self.assertRaises(FileExistsError):profile.copy_input_file(source,destination)
            self.assertEqual(destination.read_bytes(),b'concurrent evidence')
            self.assertEqual(sorted(p.name for p in root.iterdir()),['destination','source'])

    def test_pipeline_manifest_matches_clone_and_forced_copy_fallback(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'metal-pipelines';source.mkdir()
            name='a'*64+'.msl';(source/name).write_bytes(b'learned bytes')
            first=root/'clone';second=root/'copy';first.mkdir();second.mkdir()
            actual=profile.seed_pipeline_cache(source,first)
            with mock.patch.object(profile,'_clone_input_file',return_value=False):
                fallback=profile.seed_pipeline_cache(source,second)
            self.assertEqual(actual,fallback)
            self.assertEqual((first/'pipeline-cache-input-manifest.json').read_bytes(),(second/'pipeline-cache-input-manifest.json').read_bytes())

    def test_scene_guard_rejects_different_scene_but_ignores_row_padding(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);reference=root/'reference';candidate=root/'candidate'
            reference.mkdir();candidate.mkdir()
            layout=dict(format='BGRA8',origin='top_left',width=4,height=2,row_stride=20,top=[0,0,4,2],bottom=[0,2,0,2])
            for path in (reference,candidate):
                (path/'metal-benchmark-frame.json').write_text(json.dumps(layout))
            original=bytes([0,0,0,255]*4+[7]*4)*2
            (reference/'metal-benchmark-frame.bgra').write_bytes(original)
            rounded=bytearray(original);rounded[0]=1;rounded[16:20]=bytes([99]*4)
            (candidate/'metal-benchmark-frame.bgra').write_bytes(rounded)
            self.assertTrue(profile.check_scene_reference(candidate,reference)['passed'])
            (candidate/'metal-benchmark-frame.bgra').write_bytes(bytes([255]*40))
            self.assertFalse(profile.check_scene_reference(candidate,reference)['passed'])

    def test_command_timing_union_does_not_double_count_overlapping_work(self):
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder)
            (output/'metal-command-timeline-info.json').write_text(json.dumps(
                dict(retained=5,missing=0,dropped=0)))
            (output/'metal-command-timeline.csv').write_text(
                'submit_refresh,retire_refresh,producer,start_seconds,end_seconds\n'
                '0,1,0,1,10\n'       # Warmup excluded despite its long interval.
                '120,121,0,20,23\n'
                '121,122,0,21,24\n'   # Union is 20..24, not six seconds.
                '122,123,0,26,27\n'
                '600,600,0,27,28\n') # Final out-of-window capture excluded.
            result=profile.summarize_command_timeline(output,120,600)
            self.assertEqual(result['commands'],3)
            self.assertEqual(result['sum_command_seconds'],7)
            self.assertEqual(result['union_covered_seconds'],5)
            self.assertEqual(result['observed_gpu_timeline_seconds'],7)
            self.assertEqual(result['uncovered_seconds'],2)
            self.assertEqual(result['largest_gap_ms'],2000)
            self.assertTrue(result['complete_retained_data'])

    def test_command_timing_reports_dropped_data_and_rejects_invalid_timestamps(self):
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder)
            (output/'metal-command-timeline-info.json').write_text(json.dumps(
                dict(retained=1,missing=2,dropped=3)))
            path=output/'metal-command-timeline.csv'
            header='submit_refresh,retire_refresh,producer,start_seconds,end_seconds\n'
            path.write_text(header+'120,121,0,20,21\n')
            self.assertFalse(profile.summarize_command_timeline(output,120,600)['complete_retained_data'])
            for row in ('120,119,0,20,21\n','120,121,0,nan,21\n','120,121,0,22,21\n'):
                path.write_text(header+row)
                with self.assertRaises(ValueError):profile.summarize_command_timeline(output,120,600)

    def test_queue_wait_split_excludes_warmup_and_final_drain(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            (output / 'metal-queue-waits-info.json').write_text('{"dropped":0}')
            (output / 'metal-queue-waits.csv').write_text(
                'refresh_before_vblank,elapsed_ns,oldest_draw_count,'
                'oldest_command_gpu_ms,wait_after_gpu_ms\n'
                '119,9000000,64,8,1\n'
                '120,4000000,64,2,1\n'
                '120,3000000,40,6,1\n'
                '121,8000000,64,7,1\n')
            waits = {}
            result = profile.summarize_queue_wait_timeline(output, 120, 121, waits)
            self.assertEqual(waits, {121: 7})
            self.assertEqual(result['waits'], 2)
            self.assertTrue(result['complete_retained_data'])
            self.assertEqual(result['total_ms_per_refresh'], 7)
            self.assertEqual(result['split_ms_per_refresh'], dict(
                before_gpu_start=1, during_gpu_execution=4, after_gpu_end=2, unattributed=0))

    def test_queue_wait_legacy_data_stays_unattributed(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            (output / 'metal-queue-waits-info.json').write_text('{"dropped":3}')
            (output / 'metal-queue-waits.csv').write_text(
                'refresh_before_vblank,elapsed_ns,oldest_command_gpu_ms\n'
                '120,4000000,2\n')
            result = profile.summarize_queue_wait_timeline(output, 120, 121)
            self.assertFalse(result['complete_retained_data'])
            self.assertEqual(result['dropped'], 3)
            self.assertEqual(result['missing_gpu_boundaries'], 1)
            self.assertEqual(result['split_ms_per_refresh']['unattributed'], 4)

    def test_build_receipt_requires_exact_binary_identity(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);build=root/'build';output=root/'output'
            build.mkdir();output.mkdir()
            raw=json.dumps({'binary_sha256':'actual','source_revision':'historical'}).encode()
            (build/'native-metal-audit.json').write_bytes(raw)
            receipt=profile.retain_build_audit(build,output,'different')
            self.assertEqual(receipt['status'],'binary_mismatch')
            self.assertFalse((output/'native-metal-audit.json').exists())
            receipt=profile.retain_build_audit(build,output,'actual')
            self.assertEqual(receipt['build_source_revision'],'historical')
            self.assertEqual((output/'native-metal-audit.json').read_bytes(),raw)

    def test_missing_or_malformed_build_receipt_is_explicit(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            self.assertEqual(profile.retain_build_audit(root,root,'sha')['status'],'missing')
            (root/'native-metal-audit.json').write_bytes(b'{broken')
            self.assertEqual(profile.retain_build_audit(root,root,'sha')['status'],'invalid')

    def test_seed_isolated_copy_retains_only_named_guest_roots(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);seed=root/'seed';output=root/'output'
            (seed/'sdmc'/'nested').mkdir(parents=True);output.mkdir()
            (seed/'sdmc'/'nested'/'save').write_bytes(b'original')
            (seed/'config').mkdir();(seed/'config'/'old-flags').write_text('do not copy')
            digest=profile.seed_state_filesystem(seed,output)
            self.assertEqual(len(digest),64)
            self.assertFalse((output/'config').exists())
            (output/'sdmc'/'nested'/'save').write_bytes(b'changed')
            self.assertEqual((seed/'sdmc'/'nested'/'save').read_bytes(),b'original')

    def test_seed_rejects_directory_and_file_symlinks_before_copy(self):
        for directory in (False,True):
            with self.subTest(directory=directory),tempfile.TemporaryDirectory() as folder:
                root=Path(folder);seed=root/'seed';output=root/'output'
                (seed/'sdmc').mkdir(parents=True);output.mkdir()
                target=root/'external'
                if directory:target.mkdir()
                else:target.write_bytes(b'private')
                (seed/'sdmc'/'link').symlink_to(target,target_is_directory=directory)
                with self.assertRaisesRegex(ValueError,'Symlinked seed entry'):
                    profile.seed_state_filesystem(seed,output)
                self.assertEqual(list(output.iterdir()),[])

    def test_seed_manifest_identity_ignores_location_but_detects_content(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);seed=root/'seed';(seed/'nand').mkdir(parents=True)
            source=seed/'nand'/'save';source.write_bytes(b'one')
            outputs=[root/str(i) for i in range(3)]
            for output in outputs:output.mkdir()
            first=profile.seed_state_filesystem(seed,outputs[0])
            self.assertEqual(first,profile.seed_state_filesystem(seed,outputs[1]))
            source.write_bytes(b'two')
            self.assertNotEqual(first,profile.seed_state_filesystem(seed,outputs[2]))

    def test_refresh_summary_excludes_warmup_and_uses_nearest_rank(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'refresh.csv'
            path.write_text('refresh,wall_ms\n1,1000\n2,10\n3,20\n4,30\n')
            result=profile.summarize_refresh_intervals(path,1)
            self.assertEqual(result['samples'],3)
            self.assertEqual(result['mean_wall_ms'],20)
            self.assertEqual(result['p95_wall_ms'],30)
            self.assertIn('not physical scanout',result['scope'])

    def test_refresh_summary_rejects_invalid_intervals(self):
        for rows in ('1,nan\n','2,1\n1,2\n','1,-1\n'):
            with self.subTest(rows=rows),tempfile.TemporaryDirectory() as folder:
                path=Path(folder)/'refresh.csv';path.write_text('refresh,wall_ms\n'+rows)
                with self.assertRaises(ValueError):profile.summarize_refresh_intervals(path,0)

    def test_refresh_tail_compares_cpu_and_queue_for_rolling_window(self):
        wall = {frame: 10 for frame in range(1, 181)}
        wall[150] = 130
        cpu = {frame: 5 for frame in wall}
        summary = profile.summarize_refresh_tail(wall, cpu, {150: 100})
        self.assertEqual(summary['worst_rolling_120'], {
            'first_refresh': 31, 'last_refresh': 150, 'samples': 120,
            'mean_wall_ms': 11, 'thread_cpu_samples': 120, 'mean_thread_cpu_ms': 5,
            'mean_oldest_queue_wait_ms': 100 / 120,
        })
        del cpu[150]
        del wall[90]
        summary = profile.summarize_refresh_tail(wall, cpu)
        self.assertNotIn('worst_rolling_120', summary)
        self.assertNotIn('mean_thread_cpu_ms', summary['slowest_decile'])

    def test_read_refresh_cpu_intervals(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'metal-refresh-cpu.csv'
            path.write_text('refresh,thread_cpu_ms\n120,200\n121,14.5\n')
            self.assertEqual(profile.read_refresh_intervals(path,120,'thread_cpu_ms'), {121: 14.5})

    def test_pipeline_cache_copy_preserves_input_and_manifest_identity(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'prior'/'metal-pipelines'
            source.mkdir(parents=True)
            name='a'*64+'-'+'b'*64+'.msl'
            (source/name).write_bytes(b'locally learned fixture')
            first=root/'first';second=root/'second';first.mkdir();second.mkdir()
            digest=profile.seed_pipeline_cache(source,first)
            self.assertEqual(digest,profile.seed_pipeline_cache(source,second))
            copied=first/'metal-pipelines'/name
            self.assertEqual(copied.read_bytes(),(source/name).read_bytes())
            copied.write_bytes(b'new output')
            self.assertEqual((source/name).read_bytes(),b'locally learned fixture')
            manifest=json.loads((first/'pipeline-cache-input-manifest.json').read_text())
            self.assertEqual(manifest[0]['path'],'metal-pipelines/'+name)

    def test_pipeline_cache_rejects_links_foreign_files_and_directories(self):
        for kind in ('symlink','foreign','directory'):
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as folder:
                root=Path(folder);source=root/'metal-pipelines';source.mkdir()
                output=root/'output';output.mkdir()
                if kind=='symlink':
                    target=root/'outside';target.write_bytes(b'private')
                    (source/('a'*64+'.msl')).symlink_to(target)
                elif kind=='foreign':(source/'config.ini').write_text('foreign')
                else:(source/'nested').mkdir()
                with self.assertRaises(ValueError):profile.seed_pipeline_cache(source,output)
                self.assertEqual(list(output.iterdir()),[])

    def test_pipeline_cache_rejects_oversized_artifact_without_reading_it(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'metal-pipelines';source.mkdir()
            output=root/'output';output.mkdir()
            with (source/('a'*64+'.msl')).open('wb') as stream:
                stream.truncate(40*1024*1024+1)
            with self.assertRaisesRegex(ValueError,'Oversized'):
                profile.seed_pipeline_cache(source,output)
            self.assertEqual(list(output.iterdir()),[])


if __name__=='__main__':unittest.main()
