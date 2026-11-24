#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Index vertex entry points in local decrypted NCSD/NCCH or SHBIN files."""

import argparse
import mmap
from pathlib import Path
import struct


SIGNATURE = "CARA_PICA_HINTS_1"
LIMIT = 512
FINGERPRINT_BYTES = 32


def word(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def rom_range(data, title):
    if data[:4] == b"DVLB":
        if title is None:
            raise ValueError("a standalone SHBIN needs --title")
        return title, 0, len(data)

    if len(data) < 0x200:
        raise ValueError("truncated ROM header")
    ncsd = data[0x100:0x104] == b"NCSD"
    start = word(data, 0x120) * 0x200 if ncsd else 0
    if start > len(data) - 0x200 or data[start + 0x100:start + 0x104] != b"NCCH":
        raise ValueError("expected a .3ds or .cxi with an application NCCH partition")
    end = start + word(data, start + 0x104) * 0x200
    if end < start + 0x200 or end > len(data):
        raise ValueError("NCCH partition is outside the input")
    if not data[start + 0x18F] & 4:
        raise ValueError("application NCCH is encrypted")
    program_id = struct.unpack_from("<Q", data, start + 0x118)[0]
    if not program_id or (title is not None and title != program_id):
        raise ValueError("title ID does not match application NCCH")
    return program_id, start + 0x200, end


def fingerprint(data):
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def entries(data, begin, end):
    at = begin
    while (at := data.find(b"DVLB", at, end)) >= 0:
        base = at
        at += 4
        if end - base < 36:
            break
        count = word(data, base + 4)
        if not 1 <= count <= 64:
            continue
        dvlp = base + 8 + count * 4
        if dvlp + 28 > end or data[dvlp:dvlp + 4] != b"DVLP":
            continue
        code_offset, code_words, swizzle_offset, swizzle_count = struct.unpack_from(
            "<IIII", data, dvlp + 8
        )
        code = dvlp + code_offset
        code_end = code + code_words * 4
        swizzles = dvlp + swizzle_offset
        if (
            code_offset < 28 or not 1 <= code_words <= 4096 or code_end > end
            or swizzle_offset < 28 or not 1 <= swizzle_count <= 128
            or swizzles + 8 * swizzle_count > end
        ):
            continue
        records = []
        for index in range(count):
            dvle = base + word(data, base + 8 + index * 4)
            if dvle < dvlp + 28 or dvle + 64 > end or data[dvle:dvle + 4] != b"DVLE":
                break
            kind = data[dvle + 6]
            entry, terminal = struct.unpack_from("<II", data, dvle + 8)
            if kind > 1 or not entry <= terminal <= code_words or entry >= code_words:
                break
            start = code + entry * 4
            if kind == 0 and start + FINGERPRINT_BYTES <= code_end:
                records.append(fingerprint(data[start:start + FINGERPRINT_BYTES]))
        else:
            yield from records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="hint output directory")
    parser.add_argument("--title", type=lambda value: int(value, 16))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    errors = False
    for rom in args.rom:
        try:
            with rom.open("rb") as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
                title, begin, end = rom_range(data, args.title)
                distinct = set()
                for entry in entries(data, begin, end):
                    distinct.add(entry)
                    if len(distinct) == LIMIT:
                        break
            output = args.output / f"{title:016x}.pica-hints"
            temporary = output.with_suffix(".tmp")
            temporary.write_text(SIGNATURE + "\n" + "".join(f"{entry:016x}\n" for entry in sorted(distinct)))
            temporary.replace(output)
            print(f"{rom.name}: {len(distinct)} vertex fingerprints -> {output}")
        except (OSError, ValueError, struct.error) as error:
            print(f"{rom}: {error}")
            errors = True
    return int(errors)


if __name__ == "__main__":
    raise SystemExit(main())
