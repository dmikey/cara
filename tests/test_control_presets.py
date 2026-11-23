"""Check packaged preset archives independently of the UIKit editor."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import unittest
import zipfile
import zlib

ROOT=Path(__file__).resolve().parents[1]
PRESETS=ROOT/'emulator/src/ios/ControlPresets'


class ControlPresetsTests(unittest.TestCase):
    def test_registry_packages_are_valid_and_transparent(self):
        entries=json.loads((PRESETS/'registry.json').read_text())
        self.assertGreaterEqual(len(entries),4)
        self.assertEqual(len({entry['identifier'] for entry in entries}),len(entries))
        for entry in entries:
            self.assertEqual(Path(entry['file']).name,entry['file'])
            with zipfile.ZipFile(PRESETS/entry['file']) as archive:
                self.assertIsNone(archive.testzip())
                info=json.loads(archive.read('info.json'))
                self.assertEqual(info['identifier'],entry['identifier'])
                layout=info['caraActionLayout']
                self.assertEqual(layout['version'],2)
                self.assertTrue(layout['enabled'])
                for orientation in ('portrait','landscape'):
                    rep=info['representations']['ipad']['standard'][orientation]
                    self.assertEqual(layout['representations'][orientation],rep)
                    width,height=(rep['mappingSize'][key] for key in ('width','height'))
                    self.assertEqual(len(rep['screens']),2)
                    for screen in rep['screens']:
                        f=screen['outputFrame']
                        self.assertGreater(f['width'],0);self.assertGreater(f['height'],0)
                        self.assertLessEqual(f['x']+f['width'],width)
                        self.assertLessEqual(f['y']+f['height'],height)
                    controls=layout[orientation]
                    self.assertEqual(len({c['id'] for c in controls}),len(controls))
                    for control in controls:
                        frame=control['frame']
                        self.assertLessEqual(frame['x']+frame['width'],1)
                        self.assertLessEqual(frame['y']+frame['height'],1)
                        self.assertGreaterEqual(control['opacity'],.1)
                        self.assertLessEqual(control['opacity'],.9)
                # Verify the packaged backing image is transparent RGBA, including
                # its actual decoded pixels, rather than relying on its filename.
                png=archive.read('transparent.png');self.assertEqual(png[:8],b'\x89PNG\r\n\x1a\n')
                offset=8;payload=b''
                while offset<len(png):
                    length=struct.unpack('>I',png[offset:offset+4])[0]
                    kind=png[offset+4:offset+8];data=png[offset+8:offset+8+length]
                    if kind==b'IHDR': self.assertEqual(struct.unpack('>IIBBBBB',data),(2,2,8,6,0,0,0))
                    if kind==b'IDAT':payload+=data
                    offset+=12+length
                self.assertEqual(zlib.decompress(payload),bytes(18))

    def test_preset_generation_is_reproducible(self):
        before={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in PRESETS.iterdir()}
        subprocess.run([sys.executable,str(ROOT/'tools/generate_control_presets.py')],check=True)
        after={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in PRESETS.iterdir()}
        self.assertEqual(before,after)
