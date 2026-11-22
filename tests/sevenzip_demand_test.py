"""Independent raw LZMA2 7z fixtures with solid members and irregular dictionary resets."""
import lzma
import struct
import subprocess
import zlib


def uint64(value):
    for n in range(8):
        if value < 1 << (7 + 7 * n):
            return bytes([((0xff << (8-n)) & 0xff) | (value >> (8*n))]) + (value & ((1 << (8*n))-1)).to_bytes(n, "little")
    return b"\xff" + value.to_bytes(8, "little")


def archive(payload):
    prefix = b"metadata" * 8173
    data = prefix + payload
    packed = b"".join(lzma.compress(data[i:i+490007], format=lzma.FORMAT_RAW,
        filters=[dict(id=lzma.FILTER_LZMA2, dict_size=1 << 20)])[:-1] for i in range(0,len(data),490007)) + b"\x00"
    names = b"\x00" + "notes.txt\x00game.3ds\x00".encode("utf-16le")
    header = (b"\x01\x04\x06\x00\x01\x09" + uint64(len(packed)) + b"\x00" +
        b"\x07\x0b\x01\x00\x01\x21\x21\x01\x10\x0c" + uint64(len(data)) + b"\x00" +
        b"\x08\x0d\x02\x09" + uint64(len(prefix)) + b"\x0a\x01" +
        struct.pack("<II",zlib.crc32(prefix),zlib.crc32(payload)) + b"\x00\x00" +
        b"\x05\x02\x11" + uint64(len(names)) + names + b"\x00\x00")
    start = struct.pack("<QQI",len(packed),len(header),zlib.crc32(header))
    return b"7z\xbc\xaf\x27\x1c\x00\x04" + struct.pack("<I",zlib.crc32(start)) + start + packed + header


def test_demand(test, folder, payload):
    source = folder / "lzma2.7z"
    source.write_bytes(archive(payload))
    cache = folder / "7z-indices"
    samples = folder / "samples"
    positions = [0, 489999, len(payload)-32, 2000001, 16, 2097151]
    for boundary in range(490007 - len(b"metadata") * 8173, len(payload) - 32, 490007):
        positions.extend([boundary - 16, boundary, boundary + 1])
    expected = b"".join(payload[at:at+32] for at in positions)

    def run(state):
        result = subprocess.run([test, "--samples", source, cache, samples, *map(str,positions)],
                                check=True,capture_output=True,text=True)
        assert f"restart index {state}" in result.stderr
        if state == "hit":
            assert "7z progress callbacks 1" in result.stdout
        assert samples.read_bytes() == expected

    run("built")
    files = list(cache.glob("*.idx"))
    assert len(files)==1 and files[0].stat().st_size < 4096
    run("hit")
    files[0].write_bytes(b"truncated")
    run("built")
    # Replacing an archive at the same path invalidates even an equal-size game.
    new = bytearray(payload)
    new[0:32]=b"X"*32
    source.write_bytes(archive(new))
    expected = b"".join(new[at:at+32] for at in positions)
    run("built")
    # The archive header remains intact while the first packet control is invalid.
    malformed = folder / "invalid-packet.7z"
    corrupt = bytearray(source.read_bytes())
    corrupt[32] = 3
    malformed.write_bytes(corrupt)
    assert all(path.suffix == ".idx" for path in cache.iterdir())
    source.write_bytes(archive(payload))
    return source, malformed
