"""LTO build policy and generated-recipe audit, without Xcode or compiling code."""
import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('native_metal_lto_builder', ROOT / 'tools/build_native_metal.py')
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class NativeMetalLtoTests(unittest.TestCase):
    def configure(self, platform, lto):
        args = argparse.Namespace(platform=platform, cpu_jit=False, lto=lto,
                                  prefix_path=Path('/fixture/prefix'))
        return builder.configure_args(args, Path('/build'), (Path('/glslang'), Path('/spirv')),
                                      'cmake', 'ninja')

    def fixture(self, build, platform='mac', configured=False, cpu_flags='',
                metal_flags='', link_flags='', target_cpu='generic'):
        (build / 'CMakeCache.txt').write_text(
            '// Generated fixture with comments and blank lines\n\n'
            f'CARA_ENABLE_LTO:BOOL={"ON" if configured else "OFF"}\n'
            f'CARA_CPU_TARGET:STRING={target_cpu}\n'
            f'CMAKE_OBJCXX_FLAGS_RELEASE:STRING={metal_flags if metal_flags.startswith("-O") else "-O3 -DNDEBUG " + metal_flags}\n')
        target = 'cara-metal-rom' if platform == 'mac' else 'cara-ios'
        binary = f'bin/{target}.app/' + (f'Contents/MacOS/{target}' if platform == 'mac' else target)
        (build / 'build.ninja').write_text(
            'build src/core/CMakeFiles/core.dir/arm/fastinterp/fastinterp.cpp.o: CXX_COMPILER source.cpp\n'
            f'  FLAGS = -O3 -arch arm64 {cpu_flags}\n\n'
            'build src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_native_bindings.mm.o: OBJCXX_COMPILER source.mm\n'
            f'  FLAGS = -O3 -fobjc-arc {metal_flags}\n\n'
            f'build {binary}: CXX_EXECUTABLE_LINKER fixture.o\n'
            f'  FLAGS = -O3 {link_flags}\n'
            '  LINK_FLAGS = -Wl,-dead_strip\n\n')
        return target

    def test_configure_explicitly_selects_on_and_off_for_both_platforms(self):
        # Explicit OFF is essential when reusing a previously LTO-enabled cache.
        for platform in ('mac', 'ios'):
            for enabled in (False, True):
                with self.subTest(platform=platform, enabled=enabled):
                    command = self.configure(platform, enabled)
                    self.assertIn('-DCARA_ENABLE_LTO=' + ('ON' if enabled else 'OFF'), command)
                    self.assertEqual(sum(option.startswith('-DCARA_ENABLE_LTO=') for option in command), 1)
                    self.assertIn('-DCARA_CPU_TARGET=generic', command)
                    self.assertIn('-DCARA_ENABLE_JIT=OFF', command)
                    self.assertIn('-DCARA_MAC_CPU_JIT=OFF', command)

    def test_cli_configure_only_propagates_lto_and_audits_without_building(self):
        for platform in ('mac', 'ios'):
            for enabled in (False, True):
                with self.subTest(platform=platform, enabled=enabled), tempfile.TemporaryDirectory() as folder:
                    build = Path(folder)
                    argv = ['build_native_metal.py', platform, '--build-dir', str(build),
                            '--shader-sources', str(build / 'deps'), '--configure-only']
                    if platform == 'mac': argv += ['--prefix-path', '/fixture/prefix']
                    if enabled: argv.append('--lto')
                    commands = []
                    def configure(command, **_):
                        commands.append(command)
                        if command[0] == '/fixture/cmake':
                            self.assertIn('-DCARA_ENABLE_LTO=' + ('ON' if enabled else 'OFF'), command)
                            flags = '-flto=thin' if enabled else ''
                            override = next((option.partition('=')[2] for option in command
                                             if option.startswith('-DCMAKE_OBJCXX_FLAGS_RELEASE:STRING=')), '')
                            self.fixture(build, platform, enabled, flags, override, flags)
                    with patch('sys.argv', argv), patch.object(builder.platform, 'system', return_value='Darwin'), \
                         patch.object(builder.platform, 'machine', return_value='arm64'), \
                         patch.object(builder.shutil, 'which', side_effect=lambda name: '/fixture/' + name), \
                         patch.object(builder, 'pinned_source', side_effect=lambda _, dependency, __: Path('/fixture') / dependency[0]), \
                         patch.object(builder, 'run', side_effect=configure), \
                         patch.object(builder, 'bundle_and_audit') as bundle:
                        builder.main()
                    bundle.assert_not_called()
                    self.assertFalse(any('--build' in command for command in commands))
                    report = json.loads((build / 'native-metal-compiler-policy.json').read_text())
                    self.assertEqual(report['lto_requested'], enabled)

    def test_default_and_thin_lto_require_actual_complete_language_coverage(self):
        for platform in ('mac', 'ios'):
            for enabled in (False, True):
                with self.subTest(platform=platform, enabled=enabled), tempfile.TemporaryDirectory() as folder:
                    build = Path(folder)
                    flags = '-flto=thin' if enabled else ''
                    target = self.fixture(build, platform, enabled, flags, flags, flags)
                    report = builder.audit_compiler_policy(build, target, platform == 'mac', enabled)
                    self.assertEqual(report['lto_requested'], enabled)
                    self.assertEqual(report['lto_configured'], enabled)
                    self.assertEqual(report['compile_units']['fastinterp']['lto_mode'], 'thin' if enabled else None)
                    self.assertEqual(report['compile_units']['metal_bindings']['lto_mode'], 'thin' if enabled else None)
                    self.assertEqual(report['all_audited_compile_units_lto'], enabled)
                    self.assertEqual(json.loads((build / 'native-metal-compiler-policy.json').read_text()), report)

    def test_full_lto_does_not_silently_replace_requested_thin_lto(self):
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            target = self.fixture(build, configured=True, cpu_flags='-flto',
                                  metal_flags='-flto=full', link_flags='-flto')
            with self.assertRaisesRegex(RuntimeError, 'requires ThinLTO'):
                builder.audit_compiler_policy(build, target, True, True)

    def test_partial_objcxx_coverage_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            target = self.fixture(build, configured=True, cpu_flags='-flto=thin', link_flags='-flto=thin')
            with self.assertRaisesRegex(RuntimeError, 'ObjC'):
                builder.audit_compiler_policy(build, target, True, True)

    def test_owned_objcxx_flag_is_idempotent_and_default_restores_custom_flags(self):
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            original = '-O3 -DNDEBUG -fno-omit-frame-pointer -gline-tables-only'
            self.fixture(build, metal_flags=original)
            override = builder.objcxx_lto_override(build, True)
            self.assertEqual(override[0], original + ' -flto=thin')
            self.fixture(build, metal_flags=override[0])
            builder.commit_objcxx_lto_override(build, override)
            self.assertEqual(builder.objcxx_lto_override(build, True), override)
            restored = builder.objcxx_lto_override(build, False)
            self.assertEqual(restored[0], original)
            self.fixture(build, metal_flags=restored[0])
            builder.commit_objcxx_lto_override(build, restored)
            self.assertFalse((build / 'native-metal-objcxx-lto.json').exists())
            self.assertIsNone(builder.objcxx_lto_override(build, False))

    def test_restoration_preserves_trailing_custom_flags_and_rejects_unknown_edits(self):
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            self.fixture(build)
            override = builder.objcxx_lto_override(build, True)
            self.fixture(build, metal_flags=override[0])
            builder.commit_objcxx_lto_override(build, override)
            self.fixture(build, metal_flags=override[0] + ' -fno-omit-frame-pointer')
            self.assertEqual(builder.objcxx_lto_override(build, False)[0],
                             override[1]['base_release_flags'] + ' -fno-omit-frame-pointer')
            self.fixture(build, metal_flags='-O2 -DCUSTOM_USER_FLAG=1')
            with self.assertRaisesRegex(RuntimeError, 'refusing to overwrite custom flags'):
                builder.objcxx_lto_override(build, False)

    def test_stale_cache_missing_compile_flag_and_missing_link_flag_are_rejected(self):
        for configured, cpu, link, reason in (
            (False, '-flto=thin', '-flto=thin', 'Configured LTO'),
            (True, '', '-flto=thin', 'FastInterp compile'),
            (True, '-flto=thin', '', 'Executable link'),
            (True, '-flto=thin', '-flto=thin -fno-lto', 'Executable link'),
        ):
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as folder:
                build = Path(folder)
                target = self.fixture(build, configured=configured, cpu_flags=cpu, link_flags=link)
                with self.assertRaisesRegex(RuntimeError, reason):
                    builder.audit_compiler_policy(build, target, True, True)

    def test_off_policy_rejects_residual_lto_in_each_audited_stage(self):
        for cpu, metal, link, reason in (
            ('-flto=thin', '', '', 'FastInterp compile'),
            ('', '-flto=thin', '', 'ObjC'),
            ('', '', '-flto=thin', 'Executable link'),
        ):
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as folder:
                build = Path(folder)
                target = self.fixture(build, cpu_flags=cpu, metal_flags=metal, link_flags=link)
                with self.assertRaisesRegex(RuntimeError, reason):
                    builder.audit_compiler_policy(build, target, True, False)

    def test_missing_recipe_and_nonportable_cpu_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            target = self.fixture(build, target_cpu='apple-m4')
            with self.assertRaisesRegex(RuntimeError, 'generic CPU'):
                builder.audit_compiler_policy(build, target, True, False)
            self.fixture(build)
            (build / 'build.ninja').write_text('build unrelated: phony\n')
            with self.assertRaisesRegex(RuntimeError, 'Missing compiler policy build recipe'):
                builder.audit_compiler_policy(build, target, True, False)

    def test_driver_override_order_is_respected(self):
        self.assertIsNone(builder.lto_mode('-O3 -flto=thin -fno-lto'))
        self.assertEqual(builder.lto_mode('-O3 -fno-lto -flto=thin'), 'thin')


if __name__ == '__main__':
    unittest.main()
