#!/usr/bin/env python3
"""Record a CAN bus remotely from can_udp_bridge nodes.

Joins the tunnel mesh as a listen-only peer: the bridges unicast every frame
from their local CAN segment to this host, which decodes the cannelloni-style
framing and writes a log. Nothing is ever injected onto the CAN bus - the only
thing sent back is an empty keepalive.

Why keepalives: a bridge's `peer_alive` is true only while EVERY configured
peer is heard from. Adding this recorder as a peer without answering would make
that go false (status LED off, and any Home Assistant alert firing). Sending an
empty cannelloni DATA packet once a second keeps the mesh reporting healthy.
Empty packets carry no frames, so they can never reach the CAN bus.

IMPORTANT - which nodes to list: a bridge forwards only the frames it hears on
its OWN segment. Frames it receives from a peer are put on the local wire but
not re-forwarded. So to capture the whole bus, pass EVERY bridge, and add this
host to the `peers:` list of each of them.

Usage:
    python can_recorder.py 192.0.2.11 192.0.2.12
    python can_recorder.py 192.0.2.11 192.0.2.12 --asc burn.asc

Output: candump-style to stdout, and optionally a Vector ASC file (--asc) that
CANgaroo and the analysis scripts in this repo can read.
"""

import argparse
import datetime
import socket
import struct
import sys
import threading
import time

CNL_VERSION = 2
CNL_OP_DATA = 0
EFF_FLAG = 0x80000000
RTR_FLAG = 0x40000000
SFF_MASK = 0x000007FF
EFF_MASK = 0x1FFFFFFF

KEEPALIVE_INTERVAL = 1.0


def keepalive_loop(sock, peers, port, stop):
    """Announce liveness to each bridge so its peer_alive stays true."""
    seq = 0
    while not stop.is_set():
        pkt = struct.pack(">BBBH", CNL_VERSION, CNL_OP_DATA, seq & 0xFF, 0)
        for ip in peers:
            try:
                sock.sendto(pkt, (ip, port))
            except OSError:
                pass
        seq += 1
        stop.wait(KEEPALIVE_INTERVAL)


def asc_header(fh, when):
    stamp = when.strftime("%a %b %d %I:%M:%S.") + f"{when.microsecond // 1000:03d}" \
            + when.strftime(" %p %Y").lower().replace("am", "am").replace("pm", "pm")
    fh.write(f"date {stamp}\n")
    fh.write("base hex  timestamps absolute\n")
    fh.write("internal events logged\n")
    fh.write("// can_udp_bridge tools/can_recorder.py\n")
    fh.write(f"Begin Triggerblock {stamp}\n")
    fh.write("   0.000000 Start of measurement\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("peers", nargs="+",
                    help="IP of every can_udp_bridge node to record from")
    ap.add_argument("--port", type=int, default=20000)
    ap.add_argument("--asc", metavar="FILE",
                    help="also write a Vector ASC file (readable by CANgaroo)")
    ap.add_argument("--no-keepalive", action="store_true",
                    help="do not announce liveness (bridges report peer_alive false)")
    ap.add_argument("--quiet", action="store_true", help="do not print frames")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))
    sock.settimeout(1.0)

    stop = threading.Event()
    if not args.no_keepalive:
        threading.Thread(target=keepalive_loop,
                         args=(sock, args.peers, args.port, stop),
                         daemon=True).start()

    started = datetime.datetime.now()
    asc = None
    if args.asc:
        asc = open(args.asc, "w", buffering=1, encoding="utf-8")
        asc_header(asc, started)

    print(f"listening on UDP :{args.port}, recording from {', '.join(args.peers)}"
          f"{' -> ' + args.asc if args.asc else ''}   (Ctrl-C to stop)",
          file=sys.stderr)

    t0 = time.time()
    frames = keepalives = lost = 0
    last_seq = {}
    prev_t = None

    try:
        while True:
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                continue
            src = addr[0]
            if src not in args.peers:
                continue
            now = time.time()
            if len(data) < 5 or data[0] != CNL_VERSION or data[1] != CNL_OP_DATA:
                continue

            seq = data[2]
            if src in last_seq:
                gap = (seq - last_seq[src] - 1) & 0xFF
                if 0 < gap < 128:
                    lost += gap
            last_seq[src] = seq

            (count,) = struct.unpack_from(">H", data, 3)
            if count == 0:
                keepalives += 1
                continue

            pos = 5
            for _ in range(count):
                if pos + 5 > len(data):
                    break
                (raw_id,) = struct.unpack_from(">I", data, pos)
                pos += 4
                dlc = data[pos]
                pos += 1
                ext = bool(raw_id & EFF_FLAG)
                rtr = bool(raw_id & RTR_FLAG)
                can_id = raw_id & (EFF_MASK if ext else SFF_MASK)
                payload = b""
                if not rtr:
                    payload = data[pos:pos + dlc]
                    pos += dlc

                rel = now - t0
                if not args.quiet:
                    delta = (now - prev_t) * 1000 if prev_t else 0.0
                    flags = ("E" if ext else "-") + ("R" if rtr else "-")
                    id_str = f"{can_id:08X}" if ext else f"{can_id:03X}"
                    print(f"{rel:10.6f}  {delta:8.1f}ms  {src:<15} {id_str}  {flags}  "
                          f"{dlc}  {payload.hex(' ').upper()}")
                if asc:
                    hexbytes = " ".join(f"{b:02X}" for b in payload)
                    asc.write(f"{rel:11.6f} 1  {can_id:x}             Rx   d {dlc} "
                              f"{hexbytes}\n")
                prev_t = now
                frames += 1
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        if asc:
            asc.close()
        print(f"\n{frames} frames, {keepalives} keepalives, {lost} lost tunnel packets"
              + (f" -> {args.asc}" if args.asc else ""), file=sys.stderr)


if __name__ == "__main__":
    main()
