#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the real UIKit demapping/input checks on Mac Catalyst, without a simulator.

This verifies schema/archive/input/registration behavior; it does not certify
physical iPad gestures, layout appearance, or game-specific bindings.
"""
import argparse
import os
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'work/demapped-controls-check')
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('This UIKit check requires Apple Silicon macOS and full Xcode')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if 'DEVELOPER_DIR' not in env and Path('/Applications/Xcode.app/Contents/Developer').is_dir():
        env['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
    sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], env=env, text=True).strip())
    support = sdk / 'System/iOSSupport'
    framework = support / 'System/Library/Frameworks'
    if not (framework / 'UIKit.framework').is_dir():
        parser.error('The selected macOS SDK lacks Mac Catalyst UIKit support')
    source = output / 'main.mm'
    source.write_text('''#import <UIKit/UIKit.h>
#import "ios_action_layout_checks.h"
#include <cstdio>
int main() { @autoreleasepool {
    CheckActionLayout();
    std::puts("PASS: UIKit demapping checks on Mac Catalyst (not iPad gesture acceptance)");
    return 0;
} }
''')
    executable = output / 'demapped-controls-check'
    command = ['xcrun', '--sdk', 'macosx', 'clang++', '-target', 'arm64-apple-ios17.0-macabi',
               '-std=c++20', '-fobjc-arc', '-Wno-deprecated-declarations',
               '-iframework', str(framework), '-F', str(framework),
               '-isystem', str(support / 'usr/include'), '-L', str(support / 'usr/lib'),
               '-I', str(ROOT / 'emulator/src'), '-I', str(ROOT / 'tools'), str(source)]
    command += [str(ROOT / 'emulator/src/ios' / name)
                for name in ('action_layout.mm', 'skin_layout.mm', 'input_hub.mm')]
    command += ['-framework', 'UIKit', '-framework', 'GameController', '-framework', 'Foundation',
                '-framework', 'CoreGraphics', '-framework', 'UniformTypeIdentifiers', '-lz', '-o', str(executable)]
    subprocess.run(command, env=env, check=True)
    result = subprocess.run([str(executable)], env=env, capture_output=True, text=True)
    report = result.stdout + result.stderr
    (output / 'runtime.log').write_text(report)
    print(report, end='')
    result.check_returncode()


if __name__ == '__main__':
    main()
