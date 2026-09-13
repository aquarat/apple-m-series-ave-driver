#!/usr/bin/env python3
"""Append /dev/kmsg to a file, fsync'ing every line.

There is no pstore backend on this machine, so after a hard crash the only
record is what userspace had already committed to disk. Run as root in the
background before a risky step; with the driver's step_ms= markers held long
enough, the last line on disk names what killed the machine.

  sudo tools/kmsg_capture.py results/foo.kmsg &
"""
import os, sys

out = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
kmsg = os.open("/dev/kmsg", os.O_RDONLY)
os.lseek(kmsg, 0, os.SEEK_END)          # only new messages
while True:
    try:
        rec = os.read(kmsg, 8192)
    except BrokenPipeError:              # ring overwrote unread records
        continue
    if not rec:
        break
    # "prio,seq,usec,flags;text\n"
    head, _, text = rec.decode(errors="replace").partition(";")
    usec = head.split(",")[2] if head.count(",") >= 2 else "?"
    os.write(out, f"[{int(usec)/1e6:12.6f}] {text}".encode()
             if usec != "?" else rec)
    os.fsync(out)
