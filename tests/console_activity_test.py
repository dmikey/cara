"""Exercise the native desktop --activity entry point with two independent profiles."""
import base64
import datetime
import hashlib
import hmac
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

TITLE = 0x12345678
KEY = bytes(range(32))
ID = b'\xfb\xff\xff\x00pass'
FILENAME = '_' + base64.b64encode(ID).decode().rstrip('=').replace('/', '-')


def write_box(profile, outgoing):
    root = profile / 'nand/data' / ('0' * 32) / 'sysdata/00010026/00000000/CEC'
    directory = root / f'{TITLE:08x}'
    inbox = directory / 'InBox___'
    outbox = directory / 'OutBox__'
    inbox.mkdir(parents=True)
    outbox.mkdir()
    names = bytearray(0x18c)
    struct.pack_into('<H2xH2xH', names, 0, 0x6868, 1, 1)
    names[12:20] = directory.name.encode()
    (root / 'MBoxList____').write_bytes(names)
    mailbox = bytearray(96)
    struct.pack_into('<H2xI4xBB', mailbox, 0, 0x6363, TITLE, 1, 1)
    mailbox[16:48] = KEY
    (directory / 'MBoxInfo____').write_bytes(mailbox)
    (inbox / 'BoxInfo_____').write_bytes(struct.pack('<H2x7I', 0x6262, 32, 1024, 0, 1, 0, 1, 512))
    if outgoing:
        payload = b'game-produced opaque bytes'
        message = bytearray(112)
        struct.pack_into('<H2x4I', message, 0, 0x6060, 112 + len(payload) + 32, 112, len(payload), TITLE)
        message[32:40] = ID
        message[0x34] = 1
        (outbox / FILENAME).write_bytes(message + payload + hmac.digest(KEY, payload, hashlib.sha256))
        (outbox / 'OBIndex_____').write_bytes(struct.pack('<H2xI', 0x6767, 1) + ID)
    return directory


def test(executable):
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        sender, recipient = tmp / 'sender', tmp / 'recipient'
        outgoing = write_box(sender, True)
        incoming = write_box(recipient, False)
        packet = tmp / 'encounter.carapass'

        def run(profile, command, value, ok=True):
            proc = subprocess.run([executable, '--activity', profile, command, str(value)],
                                  capture_output=True, text=True)
            assert (proc.returncode == 0) == ok, (proc.stdout, proc.stderr)
            return proc.stdout

        assert run(sender, 'export', packet).startswith('1 ')
        assert KEY not in packet.read_bytes()
        bad = tmp / 'bad.carapass'
        tampered = bytearray(packet.read_bytes())
        tampered[-1] ^= 1
        bad.write_bytes(tampered)
        run(recipient, 'import', bad, False)
        assert not (incoming / 'InBox___' / FILENAME).exists()
        assert run(sender, 'import', packet).startswith('0 ')
        assert run(recipient, 'import', packet).startswith('1 ')
        dest = incoming / 'InBox___' / FILENAME
        record = dest.read_bytes()
        assert record[112:] == (outgoing / 'OutBox__' / FILENAME).read_bytes()[112:]
        assert record[0x36:0x38] == b'\1\1'
        assert struct.unpack_from('<I', record, 0x54)[0] == datetime.date.today().year
        box = (dest.parent / 'BoxInfo_____').read_bytes()
        assert box[32:] == record[:112]
        assert struct.unpack_from('<I', box, 12)[0] == len(record)
        assert (incoming / 'MBoxInfo____').read_bytes()[0x40:0x42] == b'\1\1'
        assert run(recipient, 'import', packet).startswith('0 ')
        dest.unlink()
        assert run(recipient, 'import', packet).startswith('0 ')
        assert not dest.exists()
        workers = [subprocess.Popen([executable, '--activity', recipient, 'steps', '5'],
                                    stdout=subprocess.DEVNULL) for _ in range(20)]
        assert all(worker.wait() == 0 for worker in workers)
        assert sum(int(row.split()[1]) for row in (recipient/'nand/cara-steps.txt').read_text().splitlines()) == 100
        run(recipient, 'steps', -1, False)


if __name__ == '__main__':
    test(sys.argv[1])
    print('StreetPass and pedometer profile exchange passed')
