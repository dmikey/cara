#!/usr/bin/env python3
"""Build independent Stored, Deflate, ZIP64, and malformed ZIP fixtures."""
import io
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


from sevenzip_demand_test import test_demand
from cia_rom_test import test_cia_reader


class StreamingBuffer(io.BytesIO):
    def seek(self, *args):
        raise OSError("Use ZIP data descriptors")


def z3ds_payload(payload):
    # Portable fixture with raw Zstandard blocks in independent seekable frames.
    frames, entries = [], []
    for start in range(0, len(payload), 65536):
        block = payload[start:start + 65536]
        frame = (struct.pack("<IBI", 0xfd2fb528, 0xa0, len(block)) +
                 ((len(block) << 3) | 1).to_bytes(3, "little") + block)
        frames.append(frame)
        entries.append(struct.pack("<II", len(frame), len(block)))
    table = b"".join(entries) + struct.pack("<IBI", len(frames), 0, 0x8f92eab1)
    stream = b"".join(frames) + struct.pack("<II", 0x184d2a5e, len(table)) + table
    metadata = bytes(16)
    return (struct.pack("<4s4sBBHIQQ", b"Z3DS", b"NCSD", 1, 0, 32,
                        len(metadata), len(stream), len(payload)) + metadata + stream)


def main():
    test = Path(sys.argv[1]).resolve()
    rng = random.Random(20261121)
    # A mixed corpus exercises both literal-heavy and dictionary-heavy Deflate.
    tile = rng.randbytes(65536)
    payload = b"".join(rng.randbytes(32768) + tile for _ in range(48))
    with tempfile.TemporaryDirectory(prefix="cara-zip-test-") as tmp:
        folder = Path(tmp)
        test_cia_reader(test, folder, payload)
        raw = folder / "oracle.3ds"
        raw.write_bytes(payload)
        stored = folder / "stored.zip"
        deflated = folder / "deflated.ZIP"
        zip64 = folder / "zip64.zip"
        streamed = folder / "streamed.zip"
        ambiguous = folder / "ambiguous.zip"
        malformed = folder / "malformed.zip"
        bad_offset = folder / "bad-offset.zip"
        checksum = folder / "bad-crc.zip"
        def write(target, method, extra_member=None):
            with zipfile.ZipFile(target, "w", method) as archive:
                archive.writestr("folder/Ready.3ds", payload)
                if extra_member:
                    archive.writestr(extra_member, b"metadata")

        write(stored, zipfile.ZIP_STORED)
        write(deflated, zipfile.ZIP_DEFLATED, "__MACOSX/folder/._Ready.3ds")
        original_limit = zipfile.ZIP64_LIMIT
        try:
            zipfile.ZIP64_LIMIT = 1024 * 1024
            write(zip64, zipfile.ZIP_STORED)
        finally:
            zipfile.ZIP64_LIMIT = original_limit
        buffer = StreamingBuffer()
        write(buffer, zipfile.ZIP_DEFLATED)
        streamed.write_bytes(buffer.getvalue())
        write(ambiguous, zipfile.ZIP_DEFLATED, "folder/Second.3ds")
        valid = stored.read_bytes()
        malformed.write_bytes(valid[:-5])
        corrupt = bytearray(valid)
        end = corrupt.rindex(b"PK\x05\x06")
        central, = struct.unpack_from("<I", corrupt, end + 16)
        struct.pack_into("<I", corrupt, central + 42, central + 1)
        bad_offset.write_bytes(corrupt)
        corrupt = bytearray(valid)
        name_length, extras_length = struct.unpack_from("<HH", corrupt, 26)
        corrupt[30 + name_length + extras_length + 100] ^= 0x80
        checksum.write_bytes(corrupt)
        z3ds = folder / "seekable.Z3DS"
        image = z3ds_payload(payload)
        z3ds.write_bytes(image)
        zcci = folder / "cartridge.zcci"
        zcci.write_bytes(image)
        bad_z3ds = []
        for name, position, replacement in [
            ("wrong-version", 8, b"\x02"), ("short-header", 10, bytes(2)),
            ("oversize-metadata", 12, bytes([255]) * 4),
            ("incorrect-size", 24, struct.pack("<Q", len(payload) + 1)),
            ("bogus-count", len(image) - 9, bytes([255]) * 4),
            ("bad-footer", len(image) - 1, bytes(1)),
        ]:
            member = folder / (name + ".z3ds")
            member.write_bytes(image[:position] + replacement + image[position + len(replacement):])
            bad_z3ds.append(member)
        demand, bad_packet = test_demand(test, folder, payload)
        seven_good, seven_bad = [], []
        if tar := shutil.which("bsdtar") or (shutil.which("tar") if sys.platform == "darwin" else None):
            member = folder / "Cart.3ds"
            member.write_bytes(payload)
            second = folder / "Two.cci"
            second.write_bytes(b"second")
            for name, names in [("single.7Z", [member.name]), ("multiple.7z", [member.name, second.name])]:
                target = folder / name
                subprocess.run([tar, "--format=7zip", "-cf", target, "-C", folder, *names], check=True)
                (seven_good if len(names) == 1 else seven_bad).append(target)
            truncated = folder / "truncated.7z"
            truncated.write_bytes(seven_good[0].read_bytes()[:-19])
            seven_bad.append(truncated)
        subprocess.run([test,raw,stored,deflated,zip64,streamed,z3ds,zcci,demand,*seven_good,"--bad",bad_packet,*seven_bad,malformed,bad_offset,
                        checksum,ambiguous,*bad_z3ds], check=True)


if __name__ == "__main__":
    main()
