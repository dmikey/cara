#!/usr/bin/env python3
"""Build a ROM-free UIKit fixture host; no engine or physical device required."""
import argparse
import os
from pathlib import Path
import plistlib
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
app = a.output.resolve() / 'CaraShellPreview.app'
app.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env.setdefault('DEVELOPER_DIR', '/Volumes/Fast/Applications/Xcode.app/Contents/Developer')
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphonesimulator', '--show-sdk-path'], env=env, text=True).strip()
command = ['xcrun', '--sdk', 'iphonesimulator', 'clang++', '-target', 'arm64-apple-ios17.0-simulator', '-isysroot', sdk,
           '-std=c++20', '-fobjc-arc', '-Wno-deprecated-declarations', '-I', str(ROOT / 'emulator/src'),
           str(ROOT / 'tools/ios_shell_preview.mm')]
command += [str(ROOT / 'emulator/src/ios' / name) for name in ['library_store.mm', 'artwork_store.mm', 'shell_controllers.mm', 'skin_layout.mm', 'skin_controls.mm', 'action_layout.mm', 'input_hub.mm']]
command += ['-framework', 'GameController', '-framework', 'UIKit', '-framework', 'AudioToolbox', '-framework', 'Foundation', '-framework', 'CoreGraphics', '-framework', 'QuartzCore', '-framework', 'ImageIO', '-framework', 'UniformTypeIdentifiers', '-lz', '-o', str(app / 'CaraShellPreview')]
subprocess.run(command, env=env, check=True)
info = dict(CFBundleExecutable='CaraShellPreview', CFBundleIdentifier='com.dmikey.cara.shellpreview', CFBundleName='Cara Shell Preview',
            CFBundlePackageType='APPL', CFBundleVersion='1', CFBundleShortVersionString='1.0', MinimumOSVersion='17.0',
            UIDeviceFamily=[1, 2], UILaunchScreen={}, UIFileSharingEnabled=True,
            UISupportedInterfaceOrientations=['UIInterfaceOrientationPortrait', 'UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight'],
            UIApplicationSceneManifest=dict(UIApplicationSupportsMultipleScenes=False))
info['UISupportedInterfaceOrientations~ipad'] = ['UIInterfaceOrientationPortrait', 'UIInterfaceOrientationPortraitUpsideDown', 'UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight']
(app / 'Info.plist').write_bytes(plistlib.dumps(info))
shutil.copy2(ROOT / 'emulator/src/ios/skins/CarbonDuo.deltaskin', app / 'CarbonDuo.deltaskin')
sounds = ROOT / 'emulator/src/ios/MenuSounds'
if sounds.is_dir():
    shutil.copytree(sounds, app / 'MenuSounds', dirs_exist_ok=True)
subprocess.run(['codesign', '--force', '--sign', '-', str(app)], env=env, check=True)
print(app)
