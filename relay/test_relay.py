"""Simulates two handsets against relay.py to verify the wire protocol."""
import os, socket, struct, subprocess, sys, time

HEADER = struct.Struct("<2sBBH")
REGISTER = struct.Struct("<16s16s32s")
REGISTER_ACK = struct.Struct("<B16s")
PORT = 7999
SECRET = "test-secret"

def pad(s, n): return s.encode().ljust(n, b"\x00")
def pkt(t, seq, payload=b""): return HEADER.pack(b"TB", 1, t, seq) + payload
def reg(room, dev): return pkt(1, 0, REGISTER.pack(pad(room,16), pad(dev,16), pad(SECRET,32)))

proc = subprocess.Popen([sys.executable, "relay.py", "--port", str(PORT), "--secret", SECRET,
                         "--stats-period", "0"],
                        cwd=os.path.dirname(os.path.abspath(__file__)),
                        stderr=subprocess.PIPE, text=True)
time.sleep(1.0)
relay = ("127.0.0.1", PORT)
fails = []

def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + ("  " + extra if extra else ""))
    if not cond: fails.append(name)

try:
    a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); a.settimeout(2)
    b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); b.settimeout(2)

    # 1. first handset registers, no peer yet
    a.sendto(reg("talkbox", "blue"), relay)
    data, _ = a.recvfrom(1500)
    magic, ver, typ, seq = HEADER.unpack_from(data)
    present, peer = REGISTER_ACK.unpack_from(data, HEADER.size)
    check("ack type", (magic, ver, typ) == (b"TB", 1, 2))
    check("no peer yet", present == 0)

    # 2. second registers -> both see a peer
    b.sendto(reg("talkbox", "yellow"), relay)
    data, _ = b.recvfrom(1500)
    present, peer = REGISTER_ACK.unpack_from(data, HEADER.size)
    check("yellow sees blue", present == 1 and peer.rstrip(b"\x00") == b"blue")
    a.sendto(reg("talkbox", "blue"), relay)
    data, _ = a.recvfrom(1500)
    present, peer = REGISTER_ACK.unpack_from(data, HEADER.size)
    check("blue sees yellow", present == 1 and peer.rstrip(b"\x00") == b"yellow")

    # 3. audio forwards verbatim, both directions
    audio = pkt(3, 42, bytes(range(256)) * 2)
    a.sendto(audio, relay)
    got, _ = b.recvfrom(1500)
    check("audio a->b verbatim (518 B)", got == audio, f"len={len(got)}")
    audio_b = pkt(3, 7, b"\x11" * 512)
    b.sendto(audio_b, relay)
    got, _ = a.recvfrom(1500)
    check("audio b->a verbatim", got == audio_b)

    # 4. pot forwards
    import math
    pot = pkt(4, 3, struct.pack("<f", 0.75))
    a.sendto(pot, relay)
    got, _ = b.recvfrom(1500)
    check("pot value survives", math.isclose(struct.unpack("<f", got[HEADER.size:])[0], 0.75))

    # 5. bad secret rejected
    c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); c.settimeout(2)
    bad = pkt(1, 0, REGISTER.pack(pad("talkbox",16), pad("evil",16), pad("wrong",32)))
    c.sendto(bad, relay)
    got, _ = c.recvfrom(1500)
    check("bad secret -> ERROR", got[3] == 6 and b"secret" in got[HEADER.size:])

    # 6. third device rejected
    c.sendto(reg("talkbox", "green"), relay)
    time.sleep(1.1)  # error throttle
    c.sendto(reg("talkbox", "green"), relay)
    got, _ = c.recvfrom(1500)
    check("third device -> room full", got[3] == 6 and b"full" in got[HEADER.size:])

    # 7. unregistered data sender told to re-register
    d = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); d.settimeout(2)
    d.sendto(pkt(3, 1, b"\x00" * 512), relay)
    got, _ = d.recvfrom(1500)
    check("stray audio -> not registered", got[3] == 6 and b"not registered" in got[HEADER.size:])

    # 8. NAT rebinding: same device from a new source port takes over
    a2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); a2.settimeout(2)
    a2.sendto(reg("talkbox", "blue"), relay)
    got, _ = a2.recvfrom(1500)
    present, peer = REGISTER_ACK.unpack_from(got, HEADER.size)
    check("rebound blue still paired", present == 1 and peer.rstrip(b"\x00") == b"yellow")
    b.sendto(pkt(3, 8, b"\x22" * 512), relay)
    got, _ = a2.recvfrom(1500)
    check("audio follows new endpoint", got[HEADER.size:] == b"\x22" * 512)

    # 9. garbage ignored (no crash, no reply)
    a2.sendto(b"random garbage not talkbox", relay)
    a2.sendto(b"", relay)
    time.sleep(0.3)
    a2.sendto(pkt(3, 43, b"\x33" * 512), relay)
    got, _ = b.recvfrom(1500)
    check("relay survives garbage", got[HEADER.size:] == b"\x33" * 512)

    # 10. separate rooms are isolated
    e = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); e.settimeout(1)
    f = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); f.settimeout(1)
    e.sendto(reg("other", "blue"), relay); e.recvfrom(1500)
    f.sendto(reg("other", "yellow"), relay); f.recvfrom(1500)
    e.sendto(pkt(3, 1, b"\x44" * 512), relay)
    got, _ = f.recvfrom(1500)
    isolated = got[HEADER.size:] == b"\x44" * 512
    try:
        b.recvfrom(1500); isolated = False
    except socket.timeout:
        pass
    check("rooms isolated", isolated)

    # 11. throughput: 200 audio packets in a row, none dropped
    for i in range(200):
        a2.sendto(pkt(3, 1000 + i, bytes([i & 0xFF]) * 512), relay)
    received = 0
    b.settimeout(0.5)
    try:
        while True:
            b.recvfrom(1500); received += 1
    except socket.timeout:
        pass
    check("200 packet burst relayed", received == 200, f"got={received}")

finally:
    proc.terminate()
    try:
        _, err = proc.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill(); _, err = proc.communicate()
    print("\n--- relay log ---\n" + (err or ""))

print(("ALL PASS" if not fails else "FAILURES: " + ", ".join(fails)))
sys.exit(1 if fails else 0)
