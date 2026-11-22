#!/usr/bin/env python3
"""Boot authored ARM code before and after installing an update in an isolated Mac host."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from cia_rom_test import BASE_TITLE, UPDATE_TITLE, cia_payload


def executable(reason):
    image = bytearray(0x1c00)
    image[0x100:0x104] = b"NCCH"
    struct.pack_into("<I", image, 0x104, len(image) // 512)
    for off in (0x108, 0x118, 0x400):
        struct.pack_into("<Q", image, off, BASE_TITLE)
    struct.pack_into("<I", image, 0x180, 0x400)
    image[0x18c:0x190] = bytes([1, 2, 0, 6])
    struct.pack_into("<III", image, 0x1a0, 5, 9, 1)
    image[0x200:0x208] = b"CaraTest"
    struct.pack_into("<IIII", image, 0x210, 0x100000, 1, 0x1000, 0x4000)
    for off in (0x220, 0x230):
        struct.pack_into("<III", image, off, 0x101000, 0, 0)
    image[0x40f] = 0x30
    image[0x570:0x5e0] = bytes([255]) * 0x70
    struct.pack_into("<II", image, 0x570, 0xff000100, 0xf2ffffff)
    image[0xa00:0xa05] = b".code"
    struct.pack_into("<II", image, 0xa08, 0, 0x1000)
    # mov r0, reason; svcBreak; b .  Changing the reason proves which code ran.
    struct.pack_into("<III", image, 0xc00, 0xe3a00000 | reason, 0xef00003c, 0xeafffffe)
    return bytes(image)


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="cara-cia-launch-") as work:
        folder = Path(work)
        user = folder / "user"
        user.mkdir()
        env = dict(os.environ, CARA_USER_DIR=str(user), CARA_NATIVE_DIAGNOSTICS="1",
                   CARA_NATIVE_BENCHMARK="1", CARA_NATIVE_BENCHMARK_WARMUP="3",
                   CARA_NATIVE_BENCHMARK_FRAMES="30", CARA_NATIVE_SCRIPT_ONLY_INPUT="1")

        def run(path, message, code=0):
            result = subprocess.run([binary, path], env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=45)
            assert result.returncode == code and message in result.stdout, result.stdout

        def package(name, title, image, encrypted=False):
            path = folder / name
            data = bytearray(cia_payload([(0, int(encrypted), image)], title))
            if encrypted:
                # A ticket may request a key the emulator does not provide.
                data[0x2040 + 0x140 + 0xb1] = 0xff
            if title == UPDATE_TITLE:
                # Span multiple installer blocks before the first NCCH byte.
                data[0x2040:0x2040] = bytes(0x20000)
                struct.pack_into("<I", data, 8, 0x20000)
            path.write_bytes(data)
            return path

        base = package("base.cia", BASE_TITLE, executable(2))
        run(base, "Break reason: USER")
        assert not list(user.rglob("*.app")), "Base content was installed"
        title_tree = user / "sdmc" / "Nintendo 3DS" / ("0" * 32) / ("0" * 32) / "title"
        save = title_tree / "00040000" / "0ffac000" / "data" / "00000001" / "slot-1"
        save.parent.mkdir(parents=True, exist_ok=True)
        save.write_bytes(b"existing base save")
        update = package("update.cia", UPDATE_TITLE, executable(1))
        run(update, "Game update installed")
        run(base, "Break reason: ASSERT")
        run(update, "Game update installed")  # Atomic replacement of an existing installation.
        run(package("encrypted.cia", UPDATE_TITLE, executable(0), encrypted=True), "decrypt", 3)
        run(package("bad-update.cia", UPDATE_TITLE, bytes(0x200)), "invalid", 3)
        run(base, "Break reason: ASSERT")
        assert save.read_bytes() == b"existing base save"
        assert len(list(title_tree.rglob("*.app"))) == 1
        print("PASS direct CIA boot, update overlay, repeat install, rollback and save preservation")


if __name__ == "__main__":
    main()
