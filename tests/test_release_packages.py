"""Release layout and dependency-policy fixtures; no game or compiler is invoked."""
import datetime
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
import package_mac_app
import sign_nightly
import verify_release_candidate


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

    def test_stale_ipa_source_rejected(self):
        with self.assertRaisesRegex(ValueError,'another source'):
            package_ipa.package(self.app,self.audit,self.out,expected_revision='new-source')

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

    def test_stale_build_source_rejected(self):
        report=dict(source_revision='old-source',binary_sha256=hashlib.sha256(self.core.read_bytes()).hexdigest())
        with self.assertRaisesRegex(ValueError,'another source'):
            package_mac_app.validate_build_identity(report,self.core,'new-source')

    def test_changed_build_binary_rejected(self):
        report=dict(source_revision='source',binary_sha256='wrong')
        with self.assertRaisesRegex(ValueError,'differs'):
            package_mac_app.validate_build_identity(report,self.core,'source')

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


class NightlySigningPolicyTests(unittest.TestCase):
    now = datetime.datetime(2026, 11, 20, tzinfo=datetime.timezone.utc)

    def profile(self):
        return {
            'Platform': ['iOS'],
            'TeamIdentifier': ['TEAM'],
            'ApplicationIdentifierPrefix': ['PREFIX'],
            'ExpirationDate': (self.now + datetime.timedelta(days=1)).replace(tzinfo=None),
            'Entitlements': {
                'application-identifier': 'PREFIX.com.dmikey.cara',
                'com.apple.developer.team-identifier': 'TEAM',
                'beta-reports-active': True,
                'get-task-allow': False,
                'keychain-access-groups': ['PREFIX.*'],
            },
        }

    def validate(self, profile):
        return sign_nightly.app_store_entitlements(profile, 'com.dmikey.cara', self.now)

    def test_only_required_entitlements_issued(self):
        profile = self.profile()
        profile['Entitlements']['com.apple.developer.networking.networkextension'] = ['fixture']
        entitlements = self.validate(profile)
        self.assertEqual(entitlements, {
            'application-identifier': 'PREFIX.com.dmikey.cara',
            'com.apple.developer.team-identifier': 'TEAM',
            'beta-reports-active': True,
            'get-task-allow': False,
            'keychain-access-groups': ['PREFIX.com.dmikey.cara'],
        })

    def test_device_and_enterprise_profiles_rejected(self):
        for field, value in [('ProvisionedDevices', ['fixture']), ('ProvisionsAllDevices', True)]:
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'no device identifiers'):
                self.validate(dict(self.profile(), **{field: value}))

    def test_expired_profile_rejected(self):
        profile = dict(self.profile(), ExpirationDate=self.now.replace(tzinfo=None))
        with self.assertRaisesRegex(ValueError, 'expired'):
            self.validate(profile)

    def test_wrong_app_debug_and_non_store_profiles_rejected(self):
        for field, value in [('application-identifier', 'PREFIX.*'), ('get-task-allow', True),
                             ('beta-reports-active', False), ('keychain-access-groups', ['OTHER.*'])]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                profile = self.profile()
                profile['Entitlements'][field] = value
                self.validate(profile)

    def test_signing_archive_blocks_unsafe_entries(self):
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / 'fixture.zip'
            for entries in (
                ['Payload/Cara.app/../../escape'],
                ['/Payload/Cara.app/Info.plist'],
                ['Payload/Cara.app/Test', 'Payload/Cara.app/test'],
                ['Other/Info.plist'],
            ):
                with self.subTest(entries=entries):
                    with zipfile.ZipFile(archive, 'w') as zipped:
                        for name in entries:
                            zipped.writestr(name, b'fixture')
                    with self.assertRaisesRegex(ValueError, 'Unexpected path'):
                        sign_nightly.safe_archive(archive, {'Payload'})

    def test_signing_archive_blocks_symlinks(self):
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / 'fixture.zip'
            entry = zipfile.ZipInfo('Payload/Cara.app/link')
            entry.create_system = 3
            entry.external_attr = (stat.S_IFLNK | 0o777) << 16
            with zipfile.ZipFile(archive, 'w') as zipped:
                zipped.writestr(entry, '../../escape')
            with self.assertRaisesRegex(ValueError, 'Unexpected path'):
                sign_nightly.safe_archive(archive, {'Payload'})

    def test_ci_build_number_advances_with_run(self):
        for first, second in [(1, 2), (99, 100), (9999, 10000)]:
            key = lambda run: tuple(map(int, sign_nightly.build_version(run).split('.')))
            self.assertLess(key(first), key(second))


class ReleaseCandidateTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.revision = "a" * 40
        self.certificate = "b" * 64
        self.mac = "Cara-macOS-arm64-JIT-signed.zip"
        self.ios = "Cara-iOS-arm64-GLES3-unsigned.ipa"
        self.android = ("Cara-Android-arm64-signed.apk", "Cara-Android-arm64-signed.aab")
        self.hashes = {}
        for asset in (self.mac, self.ios, *self.android):
            data = asset.encode()
            (self.root / asset).write_bytes(data)
            self.hashes[asset] = hashlib.sha256(data).hexdigest()
        self.receipt(self.mac + ".json", source_revision=self.revision, signing="developer-id",
                     notarized=True, self_contained=True, cpu_jit=True, shader_jit=False,
                     build_version="2026.11.23", asset_sha256=self.hashes[self.mac])
        self.receipt(self.ios + ".json", source_revision=self.revision, unsigned=True,
                     requires_resigning=True, renderer="gles3", bundle_id="com.dmikey.cara",
                     ipa_sha256=self.hashes[self.ios])
        self.receipt("android-signed.json", source_revision=self.revision, signing="android-upload-key",
                     certificate_sha256=self.certificate, application_id="org.cara.emu",
                     target_sdk=36, architecture="arm64-v8a", renderer="gles-atlas",
                     cpu_jit=True, shader_jit=False, version_code=100,
                     assets={asset: self.hashes[asset] for asset in self.android})

    def receipt(self, name, **data):
        (self.root / name).write_text(json.dumps(data))

    def verify(self):
        verify_release_candidate.verify(self.root, self.revision, self.certificate)

    def test_complete_candidate_verifies(self):
        self.verify()

    def test_changed_binary_is_rejected(self):
        (self.root / self.android[0]).write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            self.verify()

    def test_missing_asset_is_rejected(self):
        (self.root / self.ios).unlink()
        with self.assertRaisesRegex(ValueError, "candidate asset set"):
            self.verify()

    def test_different_ios_revision_is_rejected(self):
        name = self.ios + ".json"
        data = json.loads((self.root / name).read_text())
        self.receipt(name, **dict(data, source_revision="c" * 40))
        with self.assertRaisesRegex(ValueError, "Source mismatch"):
            self.verify()

    def test_incorrect_upload_certificate_is_rejected(self):
        self.certificate = "c" * 64
        with self.assertRaisesRegex(ValueError, "Unexpected Android upload signer"):
            self.verify()


if __name__ == '__main__':
    unittest.main()
