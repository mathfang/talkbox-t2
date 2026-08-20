#!/usr/bin/env python3
"""
TalkBox UDP relay.

Pairs two handsets that registered into the same room and forwards their audio
and potentiometer packets to each other.  Both handsets only ever send outbound
UDP to this server, so neither needs a public IP, port forwarding, or a
cooperative NAT — which is what makes calling between two arbitrary home
networks (or phone hotspots, or CGNAT) work.

    handset A --> [relay :7000] --> handset B
    handset B --> [relay :7000] --> handset A

Run it on any VPS with a public IP:

    python3 relay.py --port 7000 --secret 'change-me'

Wire format (must match the ESP32 sketches in ../os and ../os-yellow):

    header, 6 bytes, little-endian
        magic   2s  b'TB'
        version B   1
        type    B   see TYPE_* below
        seq     H   per-type sequence number, wraps at 65536

    REGISTER payload (64 bytes)
        room    16s null-padded ASCII
        device  16s null-padded ASCII
        secret  32s null-padded ASCII

    REGISTER_ACK payload (17 bytes)
        peer_present B   0 or 1
        peer_device  16s null-padded ASCII

    AUDIO / POT payloads are opaque; the relay forwards the whole datagram
    verbatim, so the sender's sequence number survives the hop.
"""

from __future__ import annotations

import argparse
import logging
import selectors
import signal
import socket
import struct
import sys
import time
from dataclasses import dataclass, field

MAGIC = b"TB"
VERSION = 1

TYPE_REGISTER = 1
TYPE_REGISTER_ACK = 2
TYPE_AUDIO = 3
TYPE_POT = 4
TYPE_BYE = 5
TYPE_ERROR = 6

TYPE_NAMES = {
    TYPE_REGISTER: "REGISTER",
    TYPE_REGISTER_ACK: "REGISTER_ACK",
    TYPE_AUDIO: "AUDIO",
    TYPE_POT: "POT",
    TYPE_BYE: "BYE",
    TYPE_ERROR: "ERROR",
}

HEADER = struct.Struct("<2sBBH")
REGISTER = struct.Struct("<16s16s32s")
REGISTER_ACK = struct.Struct("<B16s")

MAX_PACKET = 1500          # anything larger is not ours
MAX_ROOM_CLIENTS = 2
CLIENT_TIMEOUT_S = 15.0    # handsets re-register every second
REAP_PERIOD_S = 5.0
ERROR_THROTTLE_S = 1.0     # per-address rate limit on ERROR replies

Address = tuple[str, int]

log = logging.getLogger("talkbox-relay")


def unpad(raw: bytes) -> str:
    """Decodes a null-padded fixed-width ASCII field."""
    return raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")


def pad(text: str, width: int) -> bytes:
    return text.encode("ascii", errors="replace")[:width].ljust(width, b"\x00")


@dataclass
class Client:
    device: str
    room: str
    addr: Address
    last_seen: float
    packets_in: int = 0
    bytes_in: int = 0


@dataclass
class Room:
    name: str
    clients: dict[str, Client] = field(default_factory=dict)   # device -> Client


class Relay:
    def __init__(self, host: str, port: int, secret: str) -> None:
        self.secret = secret
        self.rooms: dict[str, Room] = {}
        self.by_addr: dict[Address, Client] = {}
        self.last_error_sent: dict[Address, float] = {}
        self.forwarded_packets = 0
        self.forwarded_bytes = 0
        self.dropped_packets = 0
        self.running = True

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # A generous receive buffer absorbs bursts if the process is descheduled;
        # audio is ~64 kB/s per direction.
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        except OSError:
            pass
        self.sock.bind((host, port))
        self.sock.setblocking(False)
        log.info("relay listening on %s:%d", host, port)

    # ── packet handling ──────────────────────────────────────────────────────

    def handle(self, data: bytes, addr: Address) -> None:
        if len(data) < HEADER.size or len(data) > MAX_PACKET:
            self.dropped_packets += 1
            return

        magic, version, ptype, _seq = HEADER.unpack_from(data)
        if magic != MAGIC or version != VERSION:
            self.dropped_packets += 1
            return

        payload = data[HEADER.size:]

        if ptype == TYPE_REGISTER:
            self.on_register(payload, addr)
        elif ptype in (TYPE_AUDIO, TYPE_POT):
            self.on_data(data, addr)
        elif ptype == TYPE_BYE:
            self.on_bye(addr)
        else:
            self.dropped_packets += 1

    def on_register(self, payload: bytes, addr: Address) -> None:
        if len(payload) < REGISTER.size:
            self.send_error(addr, "malformed register")
            return

        room_raw, device_raw, secret_raw = REGISTER.unpack_from(payload)
        room_name = unpad(room_raw)
        device = unpad(device_raw)
        secret = unpad(secret_raw)

        if secret != self.secret:
            self.send_error(addr, "bad secret")
            log.warning("rejected %s: bad secret (room=%r device=%r)", addr, room_name, device)
            return
        if not room_name or not device:
            self.send_error(addr, "empty room or device")
            return

        room = self.rooms.setdefault(room_name, Room(room_name))
        now = time.monotonic()
        existing = room.clients.get(device)

        if existing is None:
            # A room only ever holds the two handsets.  Expired entries were
            # already reaped, so a full room here means a genuine third party.
            if len(room.clients) >= MAX_ROOM_CLIENTS:
                self.send_error(addr, "room full")
                log.warning("rejected %s: room %r full", addr, room_name)
                return
            client = Client(device=device, room=room_name, addr=addr, last_seen=now)
            room.clients[device] = client
            self.by_addr[addr] = client
            log.info("register: %s/%s from %s:%d", room_name, device, *addr)
        else:
            if existing.addr != addr:
                # Roaming, NAT rebinding, or a reboot: follow the new endpoint.
                log.info("re-register: %s/%s moved %s:%d -> %s:%d",
                         room_name, device, *existing.addr, *addr)
                self.by_addr.pop(existing.addr, None)
                existing.addr = addr
                self.by_addr[addr] = existing
            existing.last_seen = now
            client = existing

        peer = self.peer_of(client)
        ack = REGISTER_ACK.pack(1 if peer else 0, pad(peer.device if peer else "", 16))
        self.send(addr, TYPE_REGISTER_ACK, 0, ack)

    def on_data(self, datagram: bytes, addr: Address) -> None:
        client = self.by_addr.get(addr)
        if client is None:
            # Unknown sender: probably a handset whose entry timed out. Nudge it
            # to register again (rate limited, and the reply is smaller than the
            # packet that triggered it, so it can't be used for amplification).
            self.send_error(addr, "not registered")
            self.dropped_packets += 1
            return

        client.last_seen = time.monotonic()
        client.packets_in += 1
        client.bytes_in += len(datagram)

        peer = self.peer_of(client)
        if peer is None:
            self.dropped_packets += 1
            return

        try:
            self.sock.sendto(datagram, peer.addr)
        except OSError as exc:
            log.debug("forward to %s:%d failed: %s", *peer.addr, exc)
            self.dropped_packets += 1
            return

        self.forwarded_packets += 1
        self.forwarded_bytes += len(datagram)

    def on_bye(self, addr: Address) -> None:
        client = self.by_addr.get(addr)
        if client:
            log.info("bye: %s/%s", client.room, client.device)
            self.remove(client)

    # ── helpers ──────────────────────────────────────────────────────────────

    def peer_of(self, client: Client) -> Client | None:
        room = self.rooms.get(client.room)
        if not room:
            return None
        for other in room.clients.values():
            if other.device != client.device:
                return other
        return None

    def remove(self, client: Client) -> None:
        self.by_addr.pop(client.addr, None)
        room = self.rooms.get(client.room)
        if room:
            room.clients.pop(client.device, None)
            if not room.clients:
                self.rooms.pop(client.room, None)

    def send(self, addr: Address, ptype: int, seq: int, payload: bytes) -> None:
        try:
            self.sock.sendto(HEADER.pack(MAGIC, VERSION, ptype, seq) + payload, addr)
        except OSError as exc:
            log.debug("send %s to %s:%d failed: %s", TYPE_NAMES.get(ptype, ptype), *addr, exc)

    def send_error(self, addr: Address, reason: str) -> None:
        now = time.monotonic()
        if now - self.last_error_sent.get(addr, 0.0) < ERROR_THROTTLE_S:
            return
        self.last_error_sent[addr] = now
        self.send(addr, TYPE_ERROR, 0, reason.encode("ascii")[:63])

    def reap(self) -> None:
        now = time.monotonic()
        for client in [c for c in self.by_addr.values() if now - c.last_seen > CLIENT_TIMEOUT_S]:
            log.info("timeout: %s/%s (%s:%d)", client.room, client.device, *client.addr)
            self.remove(client)

        for addr, when in [(a, t) for a, t in self.last_error_sent.items()
                           if now - t > 60.0]:
            del self.last_error_sent[addr]

    def log_stats(self, period_s: float) -> None:
        rooms = ", ".join(
            f"{name}[{'+'.join(sorted(room.clients)) or 'empty'}]"
            for name, room in sorted(self.rooms.items())
        ) or "none"
        log.info("rooms: %s | forwarded %.1f kB/s (%d pkt/s) | dropped %d",
                 rooms,
                 self.forwarded_bytes / period_s / 1000.0,
                 int(self.forwarded_packets / period_s),
                 self.dropped_packets)
        self.forwarded_packets = 0
        self.forwarded_bytes = 0
        self.dropped_packets = 0

    # ── main loop ────────────────────────────────────────────────────────────

    def run(self, stats_period_s: float) -> None:
        selector = selectors.DefaultSelector()
        selector.register(self.sock, selectors.EVENT_READ)

        next_reap = time.monotonic() + REAP_PERIOD_S
        next_stats = time.monotonic() + stats_period_s

        while self.running:
            for _key, _mask in selector.select(timeout=0.5):
                # Drain everything queued before going back to the selector.
                while True:
                    try:
                        data, addr = self.sock.recvfrom(MAX_PACKET)
                    except BlockingIOError:
                        break
                    except ConnectionResetError:
                        # Windows/ICMP artifact; ignore and keep serving.
                        continue
                    except OSError as exc:
                        log.warning("recvfrom failed: %s", exc)
                        break
                    self.handle(data, addr)

            now = time.monotonic()
            if now >= next_reap:
                self.reap()
                next_reap = now + REAP_PERIOD_S
            if stats_period_s > 0 and now >= next_stats:
                self.log_stats(stats_period_s)
                next_stats = now + stats_period_s

        selector.close()
        self.sock.close()
        log.info("relay stopped")

    def stop(self, *_args) -> None:
        self.running = False


def main() -> int:
    parser = argparse.ArgumentParser(description="TalkBox UDP relay")
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default: all)")
    parser.add_argument("--port", type=int, default=7000, help="UDP port (default: 7000)")
    parser.add_argument("--secret", default="change-me",
                        help="shared secret; must match ROOM_SECRET in the sketches")
    parser.add_argument("--stats-period", type=float, default=10.0,
                        help="seconds between stats lines, 0 to disable")
    parser.add_argument("--verbose", action="store_true", help="debug logging")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(message)s",
    )
    if args.secret == "change-me":
        log.warning("using the default secret — set --secret and ROOM_SECRET to something else")

    relay = Relay(args.host, args.port, args.secret)
    signal.signal(signal.SIGINT, relay.stop)
    signal.signal(signal.SIGTERM, relay.stop)
    relay.run(args.stats_period)
    return 0


if __name__ == "__main__":
    sys.exit(main())
