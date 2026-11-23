#!/usr/bin/env python3
"""Host simulator: dials the client's library port and pushes a small library,
byte-exact per Core/include/solarnx/wire.h (v3 handshake + records)."""
import socket
import struct
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19003
HOST = "127.0.0.1"

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
# --- HID channel: connect, send the 28-byte SessionConfig, read input frames ---
hid = socket.create_connection((HOST, 19001), timeout=5)
hid.settimeout(5)
# u32: videoBitrate, width, height, fps, audioBitrate, sampleRate, channels
hid.sendall(struct.pack("<7I", 15000, 1920, 1080, 60, 128, 48000, 2))

frames = 0
t0 = time.time()
while time.time() - t0 < 1.0:
    head = hid.recv(8)
    if len(head) != 8 or head != b"\xff" * 8:
        print("BAD MARKER:", head.hex())
        sys.exit(1)
    pkt = b""
    while len(pkt) < 224:
        chunk = hid.recv(224 - len(pkt))
        if not chunk:
            sys.exit(1)
        pkt += chunk
    if frames == 0:
        buttons, lx = struct.unpack_from("<Ii", pkt, 0)
        count, app_target, height = struct.unpack_from("<III", pkt, 196)
        print(f"frame0: buttons={buttons:#x} lx={lx} pads={count} target={app_target} h={height}")
        assert buttons & 1, "A not held"
        assert lx == -1234
        assert count == 1 and app_target == 0 and height == 1080
    frames += 1
print(f"HID: {frames} frames in 1s (~{frames} Hz), marker+224B framing OK")
hid.close()
print("pushed", len(games), "games,", len(payload), "bytes")
s.close()
