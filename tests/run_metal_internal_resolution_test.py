#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rasterize a subpixel checker and compare native guest and enhanced LCD readbacks."""
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "emulator/src"
METAL = SRC / "video_core/renderer_metal"


def main():
    with TemporaryDirectory(prefix="cara-resolution-") as tmp:
        output = Path(tmp)
        objects = []
        for name in ("pica_metal_resolution", "pica_metal_lcd"):
            obj = output / f"{name}.air"
            subprocess.run(["xcrun", "--sdk", "macosx", "metal", "-std=macos-metal2.3",
                            "-c", METAL / f"{name}.metal", "-o", obj], check=True)
            objects.append(obj)
        lib = output / "test.metallib"
        subprocess.run(["xcrun", "--sdk", "macosx", "metallib", *objects, "-o", lib], check=True)
        binary = output / "check"
        subprocess.run(["xcrun", "--sdk", "macosx", "clang++", "-std=c++20", "-fobjc-arc", "-O2",
                        "-I", SRC, "-I", ROOT / "emulator/externals/fmt/include",
                        Path(__file__).with_name("metal_internal_resolution_test.mm"),
                        METAL / "pica_metal_lcd_compose.mm", METAL / "pica_metal_resolution.mm",
                        "-framework", "Metal", "-framework", "Foundation", "-o", binary], check=True)
        subprocess.run([binary, lib], check=True, env={**os.environ, "MTL_DEBUG_LAYER": "1"})


if __name__ == "__main__":
    main()
