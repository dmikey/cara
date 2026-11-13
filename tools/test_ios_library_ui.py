#!/usr/bin/env python3
"""Build the ROM-free fixture and run its UI suite on an already booted simulator."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--device', required=True, help='Simulator UDID; never a physical device')
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
output = a.output.resolve()
output.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env.setdefault('DEVELOPER_DIR', '/Volumes/Fast/Applications/Xcode.app/Contents/Developer')
subprocess.run([sys.executable, str(ROOT / 'tools/build_ios_shell_preview.py'),
                '--output', str(output)], env=env, check=True)
subprocess.run(['xcrun', 'simctl', 'install', a.device, str(output / 'CaraShellPreview.app')],
               env=env, check=True)
subprocess.run(['xcodebuild', 'test', '-project', str(ROOT / 'tools/ios_library_ui_tests/CaraUISmoke.xcodeproj'),
               '-scheme', 'CaraUISmoke', '-destination', 'platform=iOS Simulator,id=' + a.device,
               '-derivedDataPath', str(output / 'derived'), '-resultBundlePath', str(output / 'results.xcresult'),
               'CODE_SIGNING_ALLOWED=NO'], env=env, check=True)
