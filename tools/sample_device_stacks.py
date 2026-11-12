#!/usr/bin/env python3
"""Bounded CoreDevice LLDB stack snapshots; diagnostic samples, not CPU-time weights.

Attaches to an existing PID. Never launches the app or evaluates target code.
The output retains the exact executable, UUID, hash, raw frames and LLDB log.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def command_script(config, module):
    """Let the LLDB command driver complete asynchronous CoreDevice setup."""
    return (
        "settings set target.auto-install-main-executable false\n"
        "settings set target.detach-on-error true\n"
        "settings set target.process.disable-language-runtime-unwindplans true\n"
        "target create --no-dependents=true " + json.dumps(config["binary"]) + "\n"
        "device select " + json.dumps(config["device"]) + "\n"
        "device process attach -p " + str(config["pid"]) + "\n"
        "command script import " + json.dumps(str(module)) + "\n"
        "script _sample_device_stacks_lldb.run(lldb.debugger)\nquit\n"
    )


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--device', required=True)
    p.add_argument('--pid', type=int, required=True)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--seconds', type=float, default=20)
    p.add_argument('--interval', type=float, default=0.2)
    p.add_argument('--output', type=Path, required=True, help='New output directory')
    a = p.parse_args()
    if a.pid <= 0 or not 0 < a.seconds <= 60 or not 0.05 <= a.interval <= 5:
        p.error('PID must be positive; seconds in (0,60]; interval in [0.05,5]')
    binary = a.binary.resolve(strict=True)
    a.output.mkdir(parents=True, exist_ok=False)
    output = a.output.resolve()
    saved = output / 'executable'
    shutil.copy2(binary, saved)
    env = os.environ.copy()
    env.setdefault('DEVELOPER_DIR', '/Volumes/Fast/Applications/Xcode.app/Contents/Developer')
    lldb = subprocess.check_output(['xcrun', '--find', 'lldb'], env=env, text=True).strip()
    uuid_text = subprocess.check_output(['xcrun', 'dwarfdump', '--uuid', str(saved)],
                                        env=env, text=True, timeout=10)
    uuids = [line.split()[1].upper() for line in uuid_text.splitlines() if line.startswith('UUID: ')]
    if not uuids:
        raise RuntimeError('No Mach-O UUID found')
    config = dict(device=a.device, pid=a.pid, seconds=a.seconds, interval=a.interval,
                  binary=str(saved), original_binary=str(binary), output=str(output),
                  binary_sha256=hashlib.sha256(saved.read_bytes()).hexdigest(),
                  binary_uuids=uuids, uuid_text=uuid_text, lldb=lldb,
                  diagnostic='Raw stop/resume stack snapshots; not CPU-time weights')
    (output / 'config.json').write_text(json.dumps(config, indent=2) + '\n')
    env['CARA_STACK_SAMPLE_CONFIG'] = str(output / 'config.json')
    module = Path(__file__).with_name('_sample_device_stacks_lldb.py').resolve()
    with tempfile.TemporaryDirectory(prefix='cara-stack-lldb-') as temporary:
        commands = Path(temporary) / 'commands.lldb'
        commands.write_text(command_script(config, module))
        with (output / 'lldb.log').open('w') as log:
            try:
                result = subprocess.run([lldb, '--no-lldbinit', '--batch', '-s', str(commands)],
                                        env=env, stdout=log, stderr=subprocess.STDOUT, timeout=90)
            except subprocess.TimeoutExpired:
                # Loss of debugger connection uses target.detach-on-error=true.
                raise RuntimeError('LLDB exceeded 90 seconds; inspect log and detach receipt')
    report_path = output / 'stacks.json'
    if not report_path.exists():
        raise RuntimeError('LLDB produced no report; inspect lldb.log')
    report = json.loads(report_path.read_text())
    print(f"{len(report['samples'])} diagnostic snapshots: {report_path}")
    if result.returncode or report.get('error') or not report.get('detached'):
        raise RuntimeError(report.get('error') or 'LLDB failed or detach was not confirmed')


if __name__ == '__main__':
    main()
