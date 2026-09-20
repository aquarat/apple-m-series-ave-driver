#!/usr/bin/env python3
"""Append /dev/kmsg to a file durably, and NEVER lose a record silently.

There is no pstore backend on this machine, so after a hard crash the only
record is what userspace had already committed to disk. Run as root in the
background before a risky step; the last line on disk should name what
killed the machine.

  sudo tools/kmsg_capture.py results/foo.kmsg &

The previous version had two faults that cost a day of misdiagnosis
(docs/53, 2026-09-20). It fsync'd every single record, so under a burst it
fell seconds behind the kernel; and when the ring then overwrote records it
had not read, the kernel raised EPIPE and it silently `continue`d. The
visible tail of the log was therefore wherever the reader happened to be,
not where execution stopped - and an entire analysis was built on the
difference, concluding a hang was "before stage 13" when it was in fact
after the frame.

Two changes:

  * Every gap is reported. /dev/kmsg gives a sequence number per record, so
    a gap is arithmetic, and EPIPE is recorded rather than swallowed. A log
    that lost records now says so, in the log, at the point it happened.

  * fsync is decoupled from write. Records are written immediately - so they
    survive anything short of a reset - and fsync'd when the record looks
    like a deliberate marker (STEP / stage / ===), when FSYNC_EVERY seconds
    have passed, or when the reader has caught up with the kernel. That
    keeps the durability where it matters, which is the marker before a
    dangerous access, without paying for it on every line of a register
    dump.
"""
import os
import sys
import time

FSYNC_EVERY = 0.05          # seconds; an upper bound on unsynced time
MARKERS = ("STEP", "stage ", "===")

if len(sys.argv) != 2:
    print(__doc__)
    raise SystemExit(2)

out = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
kmsg = os.open("/dev/kmsg", os.O_RDONLY | os.O_NONBLOCK)
os.lseek(kmsg, 0, os.SEEK_END)          # only new messages

last_seq = None
lost_total = 0
last_sync = time.monotonic()


def emit(line: bytes, sync: bool) -> None:
    global last_sync
    os.write(out, line)
    if sync:
        os.fsync(out)
        last_sync = time.monotonic()


while True:
    try:
        rec = os.read(kmsg, 8192)
    except BlockingIOError:
        # Caught up with the kernel: the quiet moment to make it durable.
        if time.monotonic() - last_sync > FSYNC_EVERY:
            os.fsync(out)
            last_sync = time.monotonic()
        time.sleep(0.002)
        continue
    except BrokenPipeError:
        # The ring overwrote records we had not read. Say so - loudly, in
        # the log - instead of pretending the stream is continuous.
        lost_total += 1
        emit(b"[   CAPTURE  ] *** records LOST: the kmsg ring overwrote "
             b"unread messages (EPIPE) ***\n", True)
        continue
    if not rec:
        break

    # "prio,seq,usec,flags;text\n"
    head, _, text = rec.decode(errors="replace").partition(";")
    fields = head.split(",")
    seq = int(fields[1]) if len(fields) > 1 and fields[1].isdigit() else None
    usec = fields[2] if len(fields) > 2 and fields[2].isdigit() else None

    if seq is not None and last_seq is not None and seq > last_seq + 1:
        gap = seq - last_seq - 1
        lost_total += gap
        emit(f"[   CAPTURE  ] *** {gap} record(s) LOST before seq {seq} "
             f"***\n".encode(), True)
    if seq is not None:
        last_seq = seq

    line = (f"[{int(usec) / 1e6:12.6f}] {text}".encode()
            if usec is not None else rec)
    # Markers are the whole point of this tool: sync those immediately.
    sync = (any(m in text for m in MARKERS)
            or time.monotonic() - last_sync > FSYNC_EVERY)
    emit(line, sync)
