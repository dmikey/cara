"""Release layout and dependency-policy fixtures; no game or compiler is invoked."""
import importlib.util
import hashlib
import json
from pathlib import Path
import plistlib
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import package_ipa
import mac_bundle


class IPATests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.app = self.root / 'device.app'
        self.app.mkdir()
        (self.app / 'Info.plist').write_bytes(plistlib.dumps(dict(CFBundleExecutable='cara-ios', CFBundleIdentifier='com.dmikey.cara')))
        (self.app / 'cara-ios').write_bytes(b'fixture executable')
        (self.app / 'cara-ios').chmod(0o755)
        self.audit = self.root / 'audit.json'
        self.audit.write_text(json.dumps(dict(platform='ios', jit_enabled=False, renderer='gles3', binary_sha256=hashlib.sha256(b'fixture executable').hexdigest())))
        self.out = self.root / 'dist/Cara-unsigned.ipa'

    def package(self):
        return package_ipa.package(self.app, self.audit, self.out)

    def test_payload_and_execute_permission(self):
        report = self.package()
        self.assertTrue(report['requires_resigning'])
        with zipfile.ZipFile(self.out) as archive:
            self.assertEqual(sorted(archive.namelist()), ['Payload/Cara.app/Info.plist', 'Payload/Cara.app/cara-ios'])
            self.assertTrue(archive.getinfo('Payload/Cara.app/cara-ios').external_attr >> 16 & stat.S_IXUSR)

    def test_wrong_product_rejected(self):
        for values in (dict(platform='mac', jit_enabled=False), dict(platform='ios', jit_enabled=True)):
            self.audit.write_text(json.dumps(values))
            with self.assertRaises(ValueError):
                self.package()

    def test_changed_binary_rejected(self):
        (self.app / 'cara-ios').write_bytes(b'changed fixture executable')
        with self.assertRaises(ValueError):
            self.package()

    def test_profile_not_published(self):
        (self.app / 'embedded.mobileprovision').write_bytes(b'fixture profile')
        with self.assertRaises(ValueError):
            self.package()

    def test_external_symlink_rejected(self):
        (self.app / 'external').symlink_to(self.audit)
        with self.assertRaises(ValueError):
            self.package()

    def test_executable_traversal_rejected(self):
        (self.app / 'Info.plist').write_bytes(plistlib.dumps(dict(CFBundleExecutable='../audit.json')))
        with self.assertRaises(ValueError):
            self.package()

    def test_non_executable_rejected(self):
        (self.app / 'cara-ios').chmod(0o644)
        with self.assertRaises(ValueError):
            self.package()

    def test_existing_output_preserved(self):
        self.package()
        before = self.out.read_bytes()
        with self.assertRaises(ValueError):
            self.package()
        self.assertEqual(before, self.out.read_bytes())


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.core = self.root / 'Cara.app/Contents/MacOS/cara-core'
        self.core.parent.mkdir(parents=True)
        self.core.write_bytes(b'fixture core')
        self.frameworks = self.core.parent.parent / 'Frameworks'
        self.library = self.root / 'brew/libExample.dylib'
        self.library.parent.mkdir()
        self.library.write_bytes(b'fixture library')
        self.child = self.root / 'brew/libChild.dylib'
        self.child.write_bytes(b'fixture child')

    def test_transitive_closure_and_relocation(self):
        graph = {self.core: [str(self.library)], self.library: [str(self.library), str(self.child)], self.child: [str(self.child), '/usr/lib/libSystem.B.dylib']}
        with patch.object(mac_bundle, 'dependencies', side_effect=lambda p: graph[p]), patch.object(mac_bundle, 'rpaths', return_value=[]), patch.object(mac_bundle.subprocess, 'run') as run:
            mapped = mac_bundle.bundle(self.core, self.frameworks)
        self.assertEqual(set(mapped), {self.library, self.child})
        commands = [' '.join(map(str, call.args[0])) for call in run.call_args_list]
        self.assertTrue(any('@loader_path/../Frameworks/libExample.dylib' in c for c in commands))
        self.assertTrue(any('@loader_path/libChild.dylib' in c for c in commands))

    def test_runtime_loaded_sdl3_required(self):
        self.library.write_bytes(b'@loader_path/libSDL3.dylib')
        with patch.object(mac_bundle, 'dependencies', side_effect=lambda p: [str(self.library)]), patch.object(mac_bundle, 'rpaths', return_value=[]):
            with self.assertRaisesRegex(RuntimeError, 'sdl2-compat requires'):
                mac_bundle.bundle(self.core, self.frameworks)

    def test_missing_dependency_fails(self):
        with self.assertRaisesRegex(RuntimeError, 'Unresolved'):
            mac_bundle.resolve('/no/such/lib.dylib', self.core, self.core, [])

    def test_runtime_path_resolution(self):
        with patch.object(mac_bundle, 'rpaths', return_value=[str(self.library.parent)]):
            self.assertEqual(mac_bundle.resolve('@rpath/libExample.dylib', self.core, self.core, []), self.library)

    def test_system_dependency_boundary(self):
        self.assertTrue(mac_bundle.system('/usr/lib/libc++.1.dylib'))
        self.assertFalse(mac_bundle.system('/usr/local/lib/not-system.dylib'))
        self.assertFalse(mac_bundle.system('/opt/homebrew/lib/not-system.dylib'))


if __name__ == '__main__':
    unittest.main()
