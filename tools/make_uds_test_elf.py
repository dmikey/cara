#!/usr/bin/env python3
# Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
"""Original busy-counter guest for the standalone NWM service-HLE witness.

Uses the maintained ELF generator, replacing only its SleepThread SVC with NOP.
The test driver enters this guest before invoking a real NWM Connect helper on
its Running thread. This guest itself does not issue UDS IPC; the result must be
labeled service-HLE proof rather than guest/game multiplayer compatibility.
"""
import argparse
from pathlib import Path
import struct
from make_test_elf import make_elf


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    image = bytearray(make_elf())
    assert struct.unpack_from('<I', image, 0x1000 + 6 * 4)[0] == 0xEF00000A
    struct.pack_into('<I', image, 0x1000 + 6 * 4, 0xE1A00000)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)


if __name__ == '__main__':
    main()
