#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Create an original ARM ELF fixture that increments a RAM counter between kernel sleeps."""
import argparse
from pathlib import Path
import struct

def make_elf():
    # LDR data; MOV counter; ADD; STR; LDR duration; MOV high word;
    # SVC SleepThread; B loop; data address; one millisecond in nanoseconds.
    code = struct.pack('<10I', 0xE59F3018, 0xE3A02000, 0xE2822001,
                       0xE5832000, 0xE59F000C, 0xE3A01000, 0xEF00000A,
                       0xEAFFFFF9, 0x00101000, 1_000_000)
    ident = b'\x7fELF' + bytes([1, 1, 1]) + bytes(9)
    header = ident + struct.pack('<HHIIIIIHHHHHH', 2, 40, 1, 0x00100000,
                                52, 0, 0x05000000, 52, 32, 2, 0, 0, 0)
    segment = struct.pack('<8I', 1, 0x1000, 0x00100000, 0x00100000,
                          len(code), 0x1000, 5, 0x1000)
    data_segment = struct.pack('<8I', 1, 0x2000, 0x00101000, 0x00101000,
                               4, 0x1000, 6, 0x1000)
    image = (header + segment + data_segment).ljust(0x1000, b'\0') + code
    return image.ljust(0x2000, b'\0') + bytes(4)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(make_elf())
