#!/usr/bin/env python3
"""Synthetic CPU contract for the production Metal guest-authority scan.

No GPU, emulator, ROM or platform signing is used. This cannot establish a
native renderer performance gain. Actual gameplay validation is separate.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    compiler = os.environ.get("CXX") or shutil.which("c++")
    if not compiler:
        raise RuntimeError("A C++20 compiler is required")
    with tempfile.TemporaryDirectory(prefix="cara-metal-scan-cpu-") as folder:
        binary = Path(folder) / "scan-status-test"
        subprocess.run([
            compiler, "-std=c++20", "-O2", "-Wall", "-Wextra",
            "-I" + str(root / "emulator/src"),
            "-I" + str(root / "emulator/externals/boost"),
            str(Path(__file__).with_name("metal_scan_status_test.cpp")),
            str(root / "emulator/src/video_core/renderer_metal/pica_metal_guest_authority.cpp"),
            "-o", str(binary),
        ], check=True, timeout=60)
        env = {k: v for k, v in os.environ.items() if not k.startswith("CARA_")}
        for option in ("0", "1"):
            result = subprocess.run([str(binary)], check=True, text=True,
                                    capture_output=True, timeout=10,
                                    env={**env, "CARA_METAL_REUSE_SCAN_STATUS": option,
                                         "CARA_NATIVE_DIAGNOSTICS": "1"})
            expected = f"SYNTHETIC_CPU_CONTRACT_PASS cases=8 scan_status={option}"
            if expected not in result.stdout:
                raise RuntimeError("Missing CPU contract receipt")
            print(expected)


if __name__ == "__main__":
    main()
