#!/usr/bin/env python3
"""Build independent Stored, Deflate, ZIP64, and malformed ZIP fixtures."""
import io
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import zipfile


class StreamingBuffer(io.BytesIO):
    def seek(self, *args):
        raise OSError("Use ZIP data descriptors")


def main():
    test = Path(sys.argv[1]).resolve()
    rng = random.Random(20261121)
    # A mixed corpus exercises both literal-heavy and dictionary-heavy Deflate.
    tile = rng.randbytes(65536)
    payload = b"".join(rng.randbytes(32768) + tile for _ in range(48))
    with tempfile.TemporaryDirectory(prefix="cara-zip-test-") as tmp:
        folder = Path(tmp)
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
        subprocess.run([test,raw,stored,deflated,zip64,streamed,"--bad",malformed,bad_offset,
                        checksum,ambiguous], check=True)


if __name__ == "__main__":
    main()
