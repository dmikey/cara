#!/usr/bin/env python3
"""Build only the ROM-free room screen fixture. Never opens a relay connection."""
import argparse
import os
from pathlib import Path
import plistlib
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
app = args.output.resolve() / 'CaraNetworkPreview.app'
app.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env.setdefault('DEVELOPER_DIR', '/Volumes/Fast/Applications/Xcode.app/Contents/Developer')
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphonesimulator', '--show-sdk-path'], env=env, text=True).strip()
subprocess.run(['xcrun', '--sdk', 'iphonesimulator', 'clang++', '-target', 'arm64-apple-ios17.0-simulator',
               '-isysroot', sdk, '-std=c++20', '-fobjc-arc', '-I', str(root / 'emulator/src'),
               str(root / 'tools/ios_network_ui_preview.mm'), str(root / 'emulator/src/ios/network_controller.mm'),
               '-framework', 'UIKit', '-framework', 'Foundation', '-o', str(app / 'CaraNetworkPreview')],
              env=env, check=True)
(app / 'Info.plist').write_bytes(plistlib.dumps(dict(CFBundleExecutable='CaraNetworkPreview',
    CFBundleIdentifier='com.dmikey.cara.networkpreview', CFBundleName='Cara Network UI Fixture',
    CFBundlePackageType='APPL', CFBundleVersion='1', CFBundleShortVersionString='1.0',
    MinimumOSVersion='17.0', UIDeviceFamily=[1, 2], UILaunchScreen={},
    UIApplicationSceneManifest=dict(UIApplicationSupportsMultipleScenes=False),
    UISupportedInterfaceOrientations=['UIInterfaceOrientationPortrait'])))
subprocess.run(['codesign', '--force', '--sign', '-', str(app)], env=env, check=True)
print(app)
