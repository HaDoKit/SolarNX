#!/usr/bin/env python3
"""Host simulator: dials the client's library port and pushes a small library,
byte-exact per Core/include/solarnx/wire.h (v3 handshake + records)."""
import socket
import struct
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19003

s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
s.settimeout(5)

version = struct.pack("<HH", 1, 1) + b"0.2.5-alpha".ljust(24, b"\0")
# header: magic, version(3), version[28], hash u64, layout u8 + 3 zero, revision u32, extra u32
header = (
    struct.pack("<II", 0x4C584E4F, 3)
    + version
    + struct.pack("<Q", 0x12345678ABCD)
    + struct.pack("<BBBB", 0, 0, 0, 0)
    + struct.pack("<I", 7)
    + struct.pack("<I", 0)
)
s.sendall(header)

reply = s.recv(40)
print("reply:", reply[:8].hex(), "len", len(reply))
assert len(reply) == 40 and reply[0] == 1, "client did not ask for the library"

games = [
    (100, "Cyberpunk 2077", open("/home/mihail-sisnev/SolarNX/Ui/assets/img/covers/cyberpunk2077.jpg", "rb").read()),
    (101, "Elden Ring", open("/home/mihail-sisnev/SolarNX/Ui/assets/img/covers/eldenring.jpg", "rb").read()),
    (102, "No Cover Game", b""),
]
payload = struct.pack("<I", len(games))
for gid, name, cover in games:
    payload += struct.pack("<IH", gid, len(name.encode())) + name.encode()
    payload += struct.pack("<I", len(cover)) + cover
s.sendall(payload)
print("pushed", len(games), "games,", len(payload), "bytes")
s.close()
