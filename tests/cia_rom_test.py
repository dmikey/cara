"""Construct signed-layout CIA fixtures from local bytes without copying any retail data."""
import struct
import subprocess


def pad64(data):
    return data + bytes(-len(data) % 64)


BASE_TITLE = 0x000400000ffac000
UPDATE_TITLE = BASE_TITLE | (0x0e << 32)


def cia_payload(contents, title=BASE_TITLE, boot=0):
    # The signed structures use the RSA-2048 layout. These fixtures are unsigned.
    ticket = bytearray(0x350)
    struct.pack_into(">I", ticket, 0, 0x10004)
    struct.pack_into(">Q", ticket, 0x140 + 0x9c, title)
    tmd = bytearray(0x140 + 0x9c4 + 0x30 * len(contents))
    struct.pack_into(">I", tmd, 0, 0x10004)
    struct.pack_into(">Q", tmd, 0x140 + 0x4c, title)
    struct.pack_into(">HH", tmd, 0x140 + 0x9e, len(contents), boot)
    header = bytearray(0x2020)
    data = bytearray()
    for ordinal, (index, flags, payload) in enumerate(contents):
        struct.pack_into(">IHHQ", tmd, 0x140 + 0x9c4 + ordinal * 0x30,
                         ordinal + 1, index, flags, len(payload) if payload else 512)
        if payload:
            header[0x20 + index // 8] |= 0x80 >> (index % 8)
            data.extend(payload)
    struct.pack_into("<I4x4IQ", header, 0, len(header), 0, len(ticket), len(tmd), 0, len(data))
    return pad64(header) + pad64(ticket) + pad64(tmd) + data


def test_cia_reader(test, folder, payload):
    content = bytearray(payload)
    content[0x100:0x104] = b"NCCH"
    struct.pack_into("<I", content, 0x104, len(content) // 512)
    struct.pack_into("<Q", content, 0x118, BASE_TITLE)
    content[0x18d:0x18f] = bytes([2, 0])
    oracle = folder / "cia-oracle.cxi"
    oracle.write_bytes(content)

    def put(name, data):
        path = folder / (name + ".CIA")
        path.write_bytes(data)
        return path

    # The executable follows an unrelated member and skips an absent member.
    source = cia_payload([(0, 0, bytes(512)), (1, 0, None), (3, 0, content),
                          (6, 0, bytes(512))], boot=3)
    valid = put("multi", source)
    update = put("update", cia_payload([(0, 0, content)], UPDATE_TITLE))
    invalid = []
    for name, offset, value in [
        ("oversize-ticket", 12, bytes([255]) * 4), ("unknown-ticket-signature", 0x2040, bytes(4)),
        ("mismatched-ticket", 0x2040 + 0x140 + 0x9c, bytes(8)),
        ("oversize-tmd", 16, bytes([255]) * 4), ("unlisted-content", 0x210, b"\x80"),
        ("oversize-content", 24, bytes([255]) * 8),
    ]:
        invalid.append(put(name, source[:offset] + value + source[offset + len(value):]))
    invalid.append(put("truncated", source[:-1]))
    invalid.append(put("sparse-update", cia_payload([(1, 0, content)], UPDATE_TITLE, boot=1)))
    encrypted = put("encrypted", cia_payload([(0, 1, content)]))
    wrong_title = put("wrong-title", cia_payload([(0, 0, content)], BASE_TITLE + 1))
    arguments = [test, oracle, valid, "--update-cia", update]
    for path in invalid:
        arguments.extend(["--invalid-cia", path])
    subprocess.run([*arguments, "--bad", encrypted, wrong_title, *invalid], check=True)
