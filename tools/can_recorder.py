#!/usr/bin/env python3
"""Cannelloni UDP CAN-frame recorder for the Solarfocus tunnel taps.

Listens for cannelloni v2 DATA packets (as sent by the can_udp_bridge
ESPHome component) and writes timestamped frames candump-style, both to
stdout and to a log file. Runs on plain Windows Python - no SocketCAN,
no WSL needed.

Usage:
    python can_recorder.py [--port 20000] [--out capture.log]

To capture: temporarily set the tap board's peer_ip to this PC's IP
(edit sfcantunnelN.yaml, OTA), put it in listen_only mode, and run this.

Log format (tab-separated):
    abs_time  delta_ms  can_id  flags  dlc  data_hex
delta_ms is the gap since the previous frame - the number that matters
for working out the display's poll/response timing.
"""

import argparse
import datetime
import socket
import struct
import sys

CNL_VERSION = 2
CNL_OP_DATA = 0
EFF_FLAG = 0x80000000
RTR_FLAG = 0x40000000
SFF_MASK = 0x000007FF
EFF_MASK = 0x1FFFFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=20000)
    ap.add_argument("--out", default=None, help="log file (default: capture_<ts>.log)")
    args = ap.parse_args()

    out_name = args.out or datetime.datetime.now().strftime("capture_%Y%m%d_%H%M%S.log")
    out = open(out_name, "w", buffering=1)  # line-buffered

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))
    print(f"listening on UDP :{args.port}, logging to {out_name} (Ctrl-C to stop)")

    frames = 0
    keepalives = 0
    lost = 0
    last_seq = None
    prev_t = None

    try:
        while True:
            data, addr = sock.recvfrom(2048)
            t = datetime.datetime.now()
            if len(data) < 5 or data[0] != CNL_VERSION or data[1] != CNL_OP_DATA:
                continue
            seq = data[2]
            (count,) = struct.unpack_from(">H", data, 3)
            if last_seq is not None:
                gap = (seq - last_seq - 1) & 0xFF
                if 0 < gap < 128:
                    lost += gap
            last_seq = seq

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
                    payload = data[pos : pos + dlc]
                    pos += dlc

                delta_ms = (t - prev_t).total_seconds() * 1000 if prev_t else 0.0
                prev_t = t
                flags = ("E" if ext else "-") + ("R" if rtr else "-")
                id_str = f"{can_id:08X}" if ext else f"{can_id:03X}"
                line = (
                    f"{t.strftime('%H:%M:%S.%f')[:-3]}\t{delta_ms:8.1f}\t"
                    f"{id_str}\t{flags}\t{dlc}\t{payload.hex(' ').upper()}"
                )
                print(line)
                out.write(line + "\n")
                frames += 1
    except KeyboardInterrupt:
        pass
    finally:
        out.close()
        print(
            f"\n{frames} frames, {keepalives} keepalives, "
            f"{lost} lost tunnel packets -> {out_name}",
            file=sys.stderr,
        )


if __name__ == "__main__":
    main()
