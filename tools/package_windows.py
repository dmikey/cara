#!/usr/bin/env python3
"""Package the unsigned Windows OpenGL build and its SDL runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "Cara-Windows-x86_64-OpenGL"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--sdl", type=Path, required=True)
    parser.add_argument("--zlib-license", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    source_url = "https://github.com/dmikey/cara/tree/" + revision
    receipt = {
        "source_revision": revision,
        "source_url": source_url,
        "platform": "windows",
        "architecture": "x86_64",
        "renderer": "desktop-opengl",
        "cpu_jit": False,
        "shader_jit": False,
        "signing": "unsigned",
    }
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output / f"{PACKAGE}.zip"
    with zipfile.ZipFile(output, "x", zipfile.ZIP_DEFLATED) as archive:
        archive.write(args.exe, f"{PACKAGE}/cara.exe")
        archive.write(args.sdl, f"{PACKAGE}/SDL2.dll")
        archive.write(args.sdl.parents[2] / "LICENSE.txt", f"{PACKAGE}/SDL2-LICENSE.txt")
        archive.write(args.zlib_license, f"{PACKAGE}/zlib-LICENSE.txt")
        for name in ("LICENSE", "NOTICE"):
            archive.write(ROOT / name, f"{PACKAGE}/{name}")
        for path in sorted((ROOT / "emulator" / "externals").rglob("*")):
            if path.is_file() and path.name.casefold().startswith(("license", "copying", "copyright")):
                archive.write(path, f"{PACKAGE}/licenses/{path.relative_to(ROOT / 'emulator').as_posix()}")
        archive.writestr(f"{PACKAGE}/README.txt",
                         "Run cara.exe --help for usage. A desktop OpenGL driver and the Microsoft "
                         "Visual C++ x64 runtime are required.\nCorresponding source: " + source_url + "\n")
    with output.open("rb") as file:
        receipt["asset_sha256"] = hashlib.file_digest(file, "sha256").hexdigest()
    (args.output / "windows-build.json").write_text(json.dumps(receipt, indent=2) + "\n")


if __name__ == "__main__":
    main()
