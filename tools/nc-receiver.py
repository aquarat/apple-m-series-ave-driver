#!/usr/bin/env python3
"""Netconsole receiver for the wired link from the target (AGENTS.md).

Replaces `socat -u UDP-RECV:6666 CREATE:ave-netconsole.log` on the old
receiver. Writes two files in DIR (default ~/Projects/apple-ave-driver-logging):

  ave-netconsole.log     the payload bytes exactly as received, as socat did,
                         so `grep -n 'netconsole up'` works unchanged
  ave-netconsole.ts.log  one line per datagram: arrival time (host clock,
                         CLOCK_REALTIME), sender, and the payload with its
                         trailing newline stripped - for timing the last line
                         before a hang, which the target cannot do for itself

Every datagram is flushed before the next is read. The receiver never dies
with the target, so there is nothing to fsync against.

  tools/nc-receiver.py --bind RX_IP [--port 6666] [--dir DIR]   (RX_IP: lab.env)
"""
import argparse
import os
import socket
import sys
import time

IP_FREEBIND = getattr(socket, "IP_FREEBIND", 15)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bind", required=True, help="this host's wired address")
    ap.add_argument("--port", type=int, default=6666)
    ap.add_argument("--dir", default=os.path.expanduser("~/Projects/apple-ave-driver-logging"))
    a = ap.parse_args()

    os.makedirs(a.dir, exist_ok=True)
    raw = open(os.path.join(a.dir, "ave-netconsole.log"), "ab", buffering=0)
    ts = open(os.path.join(a.dir, "ave-netconsole.ts.log"), "ab", buffering=0)

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Bind even if eth0 has no address yet (link down at boot, cable out).
    s.setsockopt(socket.IPPROTO_IP, IP_FREEBIND, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    s.bind((a.bind, a.port))
    ts.write(b"%.6f receiver up on %s:%d\n" % (time.time(), a.bind.encode(), a.port))
    print(f"listening on {a.bind}:{a.port}, writing {a.dir}", file=sys.stderr, flush=True)

    while True:
        data, (src, _) = s.recvfrom(65535)
        now = time.time()
        raw.write(data)
        ts.write(b"%.6f %s\t%s\n" % (now, src.encode(), data.rstrip(b"\n").replace(b"\n", b"\\n")))


if __name__ == "__main__":
    main()
