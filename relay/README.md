# TalkBox relay

`relay.py` lets the two handsets call each other from **different networks, any
distance apart** (California ↔ Illinois, home WiFi ↔ phone hotspot ↔ CGNAT).

Both handsets only ever send **outbound** UDP to the relay's public IP, and the
relay forwards each one's packets to the other. Nothing has to be reachable
from the internet except the VPS, so there is no port forwarding, no NAT
traversal, and no dependence on the router's cooperation. This is the same
fallback path FaceTime/WebRTC use when a direct peer-to-peer connection fails.

```
handset "blue"  (CA)  ──outbound UDP──▶  relay VPS  ◀──outbound UDP──  handset "yellow"  (IL)
                      ◀──────────────                 ──────────────▶
```

## 1. Get a VPS

Any $5/mo box with a public static IPv4 works (DigitalOcean, Linode, Hetzner,
Vultr). Python 3.9+ is all that's needed; no dependencies.

Every packet goes handset → relay → handset, so the relay's location sets the
latency. Pick one roughly between the two handsets. DigitalOcean has no
central-US region, so for California ↔ Illinois either **NYC3** or **SFO3**
works out about the same — one leg is short and the other long either way,
totalling ~70–90 ms round trip. If you want a genuinely central box, Vultr and
Linode both have Chicago and Dallas, which would shave maybe 20 ms.

## 2. Install

```bash
sudo mkdir -p /opt/talkbox && sudo cp relay.py /opt/talkbox/
```

Open the UDP port in the firewall (the relay is UDP-only — no TCP, no HTTP):

```bash
sudo ufw allow 7000/udp
```

Also open it in your provider's cloud firewall if you use one.

## 3. Run it as a service

`/etc/systemd/system/talkbox-relay.service`:

```ini
[Unit]
Description=TalkBox UDP relay
After=network-online.target

[Service]
ExecStart=/usr/bin/python3 /opt/talkbox/relay.py --port 7000 --secret 'YOUR-SECRET-HERE'
Restart=always
RestartSec=2
User=nobody
DynamicUser=yes
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload && sudo systemctl enable --now talkbox-relay
journalctl -u talkbox-relay -f
```

You should see registrations appear as each handset boots:

```
register: talkbox/blue from 73.14.x.x:6000
register: talkbox/yellow from 98.220.x.x:41830
rooms: talkbox[blue+yellow] | forwarded 64.9 kB/s (125 pkt/s) | dropped 0
```

## 4. Point the handsets at it

In **both** `os-blue/os-blue.ino` and `os-yellow/os-yellow.ino`, edit the config block at
the top:

```cpp
static const char* ROOM_SECRET = "YOUR-SECRET-HERE";   // must match --secret
static const char* RELAY_HOST  = "203.0.113.10";       // your VPS IP or DNS name
static const uint16_t RELAY_PORT = 7000;
```

`ROOM_ID` must match on both handsets; `DEVICE_ID` must differ (`blue` /
`yellow`). Set each sketch's `WIFI_SSID` / `WIFI_PASS` for whatever network that
handset is on — they no longer have to be the same network, and neither handset
needs to know the other's IP address.

Flash both, and the call comes up on its own within a second or two of the
second handset booting.

## Options

```
--host          bind address (default 0.0.0.0)
--port          UDP port (default 7000)
--secret        shared secret; must match ROOM_SECRET in the sketches
--stats-period  seconds between stats lines, 0 to disable (default 10)
--verbose       debug logging
```

## Testing

`test_relay.py` runs the relay locally and drives it with two simulated
handsets — registration, bidirectional forwarding, NAT rebinding, room
isolation, bad secrets, and a 200-packet burst:

```bash
python3 relay/test_relay.py
```

## How it works

Each handset sends a `REGISTER` packet every second carrying its room, device
name, and the shared secret. The relay records the **source address the packet
actually arrived from** — i.e. the public ip:port the handset's NAT allocated —
and replies with an ACK saying whether the other handset is present.

Audio and pot packets carry no addressing at all: the relay looks up the sender
by source address, finds the other member of that room, and forwards the
datagram verbatim. Consequences worth knowing:

- **The 1 Hz registration doubles as a NAT keepalive.** Typical UDP NAT mappings
  expire after 30–120 s of silence; refreshing every second keeps the return
  path open even when nobody is talking.
- **Re-registration follows a moved handset.** A reboot, a WiFi reconnect, or a
  NAT rebinding shows up as the same device from a new source address, and the
  relay just updates the endpoint. Nothing has to be restarted.
- **A room holds exactly two handsets.** A third gets an `ERROR` and is ignored;
  stale entries are reaped after 15 s of silence, so the slot frees itself.
- **Handsets only send audio when the ACK says the peer is online**, so an idle
  handset costs the VPS a few hundred bytes per second rather than 256 kbps.

### Bandwidth

While a call is up: 512 bytes of PCM + 6-byte header, 62.5 packets/s ≈ **33 kB/s
per direction per handset**. The relay receives both streams and sends both
streams, so it moves ~130 kB/s ≈ 1 Mbit/s total.

Providers bill **outbound** transfer only, which is half of that: ~65 kB/s, or
~235 MB per hour of talk time. Even DigitalOcean's cheapest $4 droplet (500 GB
of transfer) covers ~2100 hours a month — more than the 730 hours in a month, so
you cannot exceed it even by leaving both handsets talking 24/7. RAM and CPU are
irrelevant here; the relay holds two dictionary entries and copies packets.

### Wire format

Little-endian, shared with the sketches — change one, change the other.

| Field | Size | Notes |
|---|---|---|
| `magic` | 2 | `"TB"` |
| `version` | 1 | `1` |
| `type` | 1 | 1 REGISTER, 2 REGISTER_ACK, 3 AUDIO, 4 POT, 5 BYE, 6 ERROR |
| `seq` | 2 | per-type counter, wraps at 65536 |

`REGISTER` payload: `room[16] device[16] secret[32]`, null-padded ASCII.
`REGISTER_ACK` payload: `peer_present[1] peer_device[16]`.
`AUDIO` payload: 512 bytes of 16 kHz / 16-bit / mono PCM.
`POT` payload: one little-endian `float`, 0.0–1.0.

The secret is a plaintext shared string, enough to keep stray internet scan
traffic out of your room. It is not encryption — the audio crosses the internet
in the clear, so treat these handsets as a fun open channel, not a private one.

## Troubleshooting

The handsets show link state on their own LEDs whenever a call is *not* up, so
you can diagnose from either end without a laptop:

| LEDs | Meaning |
|---|---|
| Slow red pulse | WiFi not associated — check SSID/password |
| Slow amber pulse | WiFi up but relay unreachable or unresolved — check `RELAY_HOST`, firewall |
| Slow blue pulse | Registered with the relay, waiting for the other handset |
| Steady amber, tracks the far pot | Call is up |

The serial monitor (115200) prints a line every 2 s:

```
[link=up rssi=-58] tx=33280 B/s rx=33280 B/s | jitter=118 ms lost=0 late=0 under=0 over=0 | heap=142312
```

- `lost` climbing → packet loss on the path. A few per second is survivable;
  sustained loss means a weak WiFi link or a congested uplink.
- `under` climbing → the jitter buffer keeps running dry. Raise
  `JITTER_PREFILL_MS` in the sketch (each 10 ms costs 10 ms of one-way latency).
- `jitter` growing steadily over a long call → clock drift; the buffer trims
  itself at `JITTER_MAX_MS`, which you'll hear as a rare tiny skip.
- `link=waiting-peer` on both handsets → they registered into different rooms,
  or with different secrets.
