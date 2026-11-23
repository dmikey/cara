"""Verify release source export against an actual isolated Git repository."""
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import package_source


class SourceArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'repo'
        self.root.mkdir()
        self.output = Path(self.temp.name) / 'dist/source.tar.gz'
        for name in ('LICENSE', 'NOTICE', 'tools/build.py', 'emulator/VENDORED_SOURCE.json',
                     'emulator/src/ios/main.mm', 'emulator/externals/soundtouch/COPYING.txt'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('original fixture ' + name)
        self.git('init', '-q')
        self.git('add', '.')
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'fixture')
        self.revision = self.git('rev-parse', 'HEAD').strip()
        self.override = patch.object(package_source, 'ROOT', self.root)
        self.override.start()
        self.addCleanup(self.override.stop)

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.root, text=True)

    def test_archive_contains_exact_tracked_source(self):
        result = package_source.package(self.output, self.revision)
        self.assertEqual(result['source_revision'], self.revision)
        with tarfile.open(self.output) as archive:
            source = archive.extractfile('Cara-source-' + self.revision + '/tools/build.py').read()
            self.assertEqual(source, (self.root / 'tools/build.py').read_bytes())
        self.assertEqual(json.loads(self.output.with_suffix('.gz.json').read_text()), result)

    def test_dirty_source_rejected(self):
        (self.root / 'NOTICE').write_text('uncommitted changes')
        with self.assertRaisesRegex(ValueError, 'clean checkout'):
            package_source.package(self.output, self.revision)

    def test_wrong_revision_rejected(self):
        with self.assertRaisesRegex(ValueError, 'differs from checkout'):
            package_source.package(self.output, '0' * 40)
