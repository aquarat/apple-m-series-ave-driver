#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Extract the AVE firmware's pristine DATA segment for fw_restore_data.

macOS restores a pristine copy of the firmware's DATA segment over physical DRAM
before *every* start of the AVE core (13.5 AVE_Firmware::UpdateImage; 26.6.2
AVE_FwImg::UpdateImage -> RestoreCTRRData). Our driver's fw_restore_data option
does the same, and this script produces the blob it restores from.

The authoritative pristine bytes are what iBoot left in DRAM before any start
was issued - which is exactly what macOS snapshots. That is captured in
data/blobs/iboot-window-16m.bin, a 16 MiB read of physical memory from the
firmware's TEXT base (0x10000b28000) taken before the core was ever started.
We do NOT extract from the shipped Mach-O (data/blobs/macos-13.5/ave_h13c.bin):
its file-backed DATA differs from what the core actually sees by 147 bytes that
iBoot fills in at boot (the RTKit tag list, e.g. the IOBA I/O base 0x40c000000).
macOS snapshots memory *after* those fills, so the memory dump is correct and
the file is not.

Layout (all confirmed against the dump):
  window base phys   = 0x10000b28000  (AVE_IBOOT_TEXT_PHYS)
  DATA base phys     = 0x10001a90000  (AVE_IBOOT_DATA_PHYS)
  DATA vmsize        = 0x134000       (AVE_IBOOT_DATA_SIZE)
  DATA offset in win = 0x00f68000     (DATA - window base)
  DATA bytes in win  = 0x00098000     (window end - DATA offset; exactly to end)

Only the first 0x98000 of DATA's 0x134000 bytes fall inside the 16 MiB window.
DATA 0x64000..0x98000 is already all-zero in the dump, and 0x98000..0x134000 is
bss beyond the window, assumed zero. So the pristine image is: the 0x98000 bytes
from the dump, zero-padded out to the full 0x134000 vmsize.

Output: data/blobs/ave-data-pristine.bin (0x134000 bytes). It stays gitignored.
Install it for the driver as:
  /lib/firmware/apple/ave-data-pristine.bin
(the default fw_restore_path; override with the module parameter).
"""

import argparse
import os
import sys

WINDOW_PHYS = 0x10000b28000
DATA_PHYS   = 0x10001a90000
DATA_SIZE   = 0x134000

DATA_OFF_IN_WINDOW = DATA_PHYS - WINDOW_PHYS   # 0xf68000


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--window", default=os.path.join(repo, "data/blobs/iboot-window-16m.bin"),
                    help="16 MiB physical dump from the firmware TEXT base")
    ap.add_argument("-o", "--output", default=os.path.join(repo, "data/blobs/ave-data-pristine.bin"),
                    help="output blob (0x134000 bytes, zero-padded)")
    args = ap.parse_args()

    win_size = os.path.getsize(args.window)
    covered = win_size - DATA_OFF_IN_WINDOW
    if covered <= 0:
        sys.exit("window %s (%#x bytes) does not reach DATA at offset %#x"
                 % (args.window, win_size, DATA_OFF_IN_WINDOW))
    if covered > DATA_SIZE:
        covered = DATA_SIZE

    with open(args.window, "rb") as f:
        f.seek(DATA_OFF_IN_WINDOW)
        data = f.read(covered)
    if len(data) != covered:
        sys.exit("short read from %s" % args.window)

    # Sanity: iBoot fills IOBA with the AP-physical I/O base. If the tag is
    # zero the dump was taken before iBoot ran, and this is not pristine DATA.
    tag = data.find(b"ABOI")   # "IOBA", little-endian in memory
    if tag < 0:
        print("WARNING: no IOBA tag in DATA - is this really an AVE image dump?",
              file=sys.stderr)
    else:
        payload = int.from_bytes(data[tag + 8:tag + 16], "little")
        print("IOBA at DATA+%#x payload %#x" % (tag, payload))
        if payload == 0:
            print("WARNING: IOBA payload is 0 - dump taken before iBoot filled it",
                  file=sys.stderr)

    out = bytearray(DATA_SIZE)
    out[:len(data)] = data
    with open(args.output, "wb") as f:
        f.write(out)

    print("wrote %s: %#x bytes (%#x from dump, %#x zero-padded bss)"
          % (args.output, len(out), len(data), DATA_SIZE - len(data)))
    print("install as /lib/firmware/apple/ave-data-pristine.bin for fw_restore_data")


if __name__ == "__main__":
    main()
