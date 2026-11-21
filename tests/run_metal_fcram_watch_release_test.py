#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check Metal ownership and CPU page barriers against the native Mac build."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

from run_metal_async_depth_validation_test import ninja_fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--compile-only", action="store_true")
    options = parser.parse_args()
    build = options.build_dir.resolve()
    graph = (build / "build.ninja").read_text()
    renderer = ninja_fields(
        graph, "src/video_core/CMakeFiles/video_core.dir/renderer_metal/pica_metal_native_renderer.mm.o"
    )
    app = ninja_fields(graph, "bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom")
    flags = [flag for key in ("DEFINES", "FLAGS", "INCLUDES") for flag in shlex.split(renderer[key])]
    source = Path(__file__).with_name("metal_fcram_watch_release_test.mm").resolve()
    obj = build / "fcram-watch-test.o"
    binary = build / "fcram-watch-test"
    subprocess.run(["xcrun", "clang++", *flags, "-c", str(source), "-o", str(obj)], cwd=build, check=True)
    subprocess.run(["xcrun", "clang++", "-Wl,-dead_strip", str(obj),
                    *shlex.split(app["LINK_LIBRARIES"]), "-o", str(binary)], cwd=build, check=True)
    if options.compile_only:
        return
    for full, partial in (("0", "0"), ("1", "0"), ("0", "1"), ("1", "1")):
        env = {key: value for key, value in os.environ.items() if not key.startswith("CARA_")}
        env.update(CARA_METAL_RELEASE_INVALIDATED_FCRAM_PAGES=full,
                   CARA_METAL_RELEASE_OWNERLESS_FCRAM_PAGES=partial, MTL_DEBUG_LAYER="1")
        subprocess.run([str(binary), str(build / "src/video_core/cara-pica.metallib")],
                       cwd=build, check=True, env=env, timeout=30)


if __name__ == "__main__":
    main()
