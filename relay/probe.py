#!/usr/bin/env python3
"""
Pretends to be a handset, so you can test a relay from a laptop.

Sends a REGISTER to the relay and prints the ACK, which tells you three things
without touching the ESP32s: the port is open, the relay is running, and your
secret matches.

    python3 probe.py 203.0.113.10 --secret 'change-me'

Leave it running while you boot a real handset and it will report the moment
the relay pairs you with it — useful for confirming which end is at fault.
"""

import argparse
import socket
import struct
import sys
import time

HEADER = struct.Struct("<2sBBH")
REGISTER = struct.Struct("<16s16s32s")
REGISTER_ACK = struct.Struct("<B16s")
TYPE_REGISTER, TYPE_REGISTER_ACK, TYPE_ERROR = 1, 2, 6


def main() -> int:
    ap = argparse.ArgumentParser(description="Probe a TalkBox relay")
    ap.add_argument("host", help="relay IP or hostname")
    ap.add_argument("--port", type=int, default=7000)
    ap.add_argument("--secret", default="change-me")
    ap.add_argument("--room", default="talkbox")
    ap.add_argument("--device", default="probe", help="must differ from both handsets")
    ap.add_argument("--count", type=int, default=5, help="registrations to send, 0 = forever")
    args = ap.parse_args()

    def pad(s: str, n: int) -> bytes:
        return s.encode("ascii")[:n].ljust(n, b"\x00")

    packet = HEADER.pack(b"TB", 1, TYPE_REGISTER, 0) + REGISTER.pack(
        pad(args.room, 16), pad(args.device, 16), pad(args.secret, 32))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2.0)
    target = (args.host, args.port)
    print(f"probing {args.host}:{args.port} as {args.room}/{args.device}\n")

    replies = 0
    acks = 0
    sent = 0
    while args.count == 0 or sent < args.count:
        sock.sendto(packet, target)
        sent += 1
        try:
            data, _ = sock.recvfrom(1500)
        except socket.timeout:
            print("  no reply — port blocked, wrong IP, or relay not running")
            time.sleep(1.0)
            continue

        replies += 1
        magic, version, ptype, _seq = HEADER.unpack_from(data)
        if magic != b"TB" or version != 1:
            print(f"  reply is not TalkBox traffic: {data[:32]!r}")
        elif ptype == TYPE_ERROR:
            print(f"  relay rejected us: {data[HEADER.size:].decode('ascii', 'replace')}")
        elif ptype == TYPE_REGISTER_ACK:
            acks += 1
            present, peer = REGISTER_ACK.unpack_from(data, HEADER.size)
            peer_name = peer.split(b"\x00", 1)[0].decode("ascii", "replace")
            print(f"  ACK — peer_present={present}"
                  + (f", paired with \"{peer_name}\"" if present else ", no handset online yet"))
        else:
            print(f"  unexpected packet type {ptype}")
        time.sleep(1.0)

    print()
    if acks:
        print(f"relay reachable and accepting us ({acks}/{sent} ACKs). "
              f"Set RELAY_HOST = \"{args.host}\" in both sketches.")
        return 0
    if replies:
        print(f"relay is reachable ({replies}/{sent} replies) but REJECTED the registration "
              "— see the reason above. The network path is fine; fix the room/secret so it\n"
              "matches the relay's --secret, then probe again.")
        return 1
    print("relay unreachable. Check, in order:\n"
          "  1. the relay is running     (journalctl -u talkbox-relay -f, or run it in a terminal)\n"
          "  2. the host firewall        (sudo ufw allow 7000/udp)\n"
          "  3. the provider's firewall  (DigitalOcean/AWS cloud firewall, UDP 7000 inbound)\n"
          "  4. the IP and port are right")
    return 1


if __name__ == "__main__":
    sys.exit(main())
