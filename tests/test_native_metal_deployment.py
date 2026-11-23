"""Native-Metal artifact deployment policy, independent of a live Apple SDK."""
import argparse
import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('native_metal_builder', ROOT / 'tools/build_native_metal.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class NativeMetalDeploymentTests(unittest.TestCase):
    def test_native_configuration_uses_reflection_api_floor(self):
        for platform, minimum in (('mac', '13.0'), ('ios', '16.0')):
            args = argparse.Namespace(platform=platform, cpu_jit=False, lto=False, prefix_path=Path('/fixture/prefix'))
            command = builder.configure_args(args, Path('/build'), (Path('/glslang'), Path('/spirv')), 'cmake', 'ninja')
            self.assertIn('-DCMAKE_OSX_DEPLOYMENT_TARGET=' + minimum, command)

    def test_actual_newer_binary_wins_over_plist_floor(self):
        with tempfile.TemporaryDirectory() as folder:
            app = Path(folder) / 'Cara.app'
            app.mkdir()
            info = app / 'Info.plist'
            info.write_bytes(plistlib.dumps(dict(MinimumOSVersion='15.0')))
            with patch.object(builder, 'minimum_os', return_value='27.2'):
                minimum = builder.update_bundle_minimum(app, app / 'cara-ios', False, ['/usr/lib/libc++.1.dylib'])
            self.assertEqual(minimum, '27.2')
            self.assertEqual(plistlib.loads(info.read_bytes())['MinimumOSVersion'], '27.2')

    def test_native_ios_never_advertises_unsupported_ios15(self):
        with tempfile.TemporaryDirectory() as folder:
            app = Path(folder) / 'Cara.app'
            app.mkdir()
            info = app / 'Info.plist'
            info.write_bytes(plistlib.dumps(dict(MinimumOSVersion='15.0')))
            with patch.object(builder, 'minimum_os', return_value='16.0'):
                builder.update_bundle_minimum(app, app / 'cara-ios', False, [])
            self.assertEqual(plistlib.loads(info.read_bytes())['MinimumOSVersion'], '16.0')

    def test_mac_dependency_can_raise_artifact_minimum(self):
        with tempfile.TemporaryDirectory() as folder:
            app = Path(folder) / 'Cara.app'
            (app / 'Contents').mkdir(parents=True)
            info = app / 'Contents/Info.plist'
            info.write_bytes(plistlib.dumps(dict(LSMinimumSystemVersion='13.0')))
            binary = app / 'Contents/MacOS/cara-metal-rom'
            dependency = Path(folder) / 'libSDL2.dylib'
            dependency.write_bytes(b'fixture')
            with patch.object(builder, 'minimum_os', side_effect=lambda path: '15.0' if path == dependency.resolve() else '13.0'), patch.object(builder, 'linked_libraries', return_value=[]):
                minimum = builder.update_bundle_minimum(app, binary, True, [str(dependency), '/usr/lib/libc++.1.dylib'])
            self.assertEqual(minimum, '15.0')
            self.assertEqual(plistlib.loads(info.read_bytes())['LSMinimumSystemVersion'], '15.0')

    def test_transitive_mac_dependency_can_raise_artifact_minimum(self):
        with tempfile.TemporaryDirectory() as folder:
            app = Path(folder) / 'Cara.app'
            (app / 'Contents').mkdir(parents=True)
            info = app / 'Contents/Info.plist'
            info.write_bytes(plistlib.dumps(dict(LSMinimumSystemVersion='13.0')))
            first, second = Path(folder) / 'libSDL2.dylib', Path(folder) / 'libSupport.dylib'
            first.write_bytes(b'fixture'); second.write_bytes(b'fixture')
            def libraries(path):
                return [str(first), str(second)] if path == first.resolve() else [str(second)]
            with patch.object(builder, 'minimum_os', side_effect=lambda path: '26.0' if path == second.resolve() else '13.0'), patch.object(builder, 'linked_libraries', side_effect=libraries):
                minimum = builder.update_bundle_minimum(app, app / 'Contents/MacOS/cara-metal-rom', True, [str(first)])
            self.assertEqual(minimum, '26.0')

    def test_unresolved_mac_deployment_dependency_is_not_guessed(self):
        with patch.object(builder, 'minimum_os', return_value='13.0'):
            with self.assertRaisesRegex(RuntimeError, 'Cannot audit'):
                builder.update_bundle_minimum(Path('/app'), Path('/binary'), True, ['@rpath/libSDL2.dylib'])

    def test_missing_load_command_is_rejected(self):
        with patch.object(builder, 'read', return_value='not a Mach-O deployment load command'):
            with self.assertRaisesRegex(RuntimeError, 'Missing deployment'):
                builder.minimum_os(Path('/fixture'))
