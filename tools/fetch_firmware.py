#!/usr/bin/env python3
"""Fetch Apple's AVE firmware and DeviceTree out of an IPSW without downloading it.

An Apple Silicon restore image is ~20 GB, but the objects we need total under
3 MB. A restore IPSW is a plain ZIP, so HTTP range requests let us read the
central directory and extract only the members we want.

Nothing downloaded here may be redistributed: these are Apple-proprietary
blobs. They land in data/blobs/, which is gitignored. This mirrors what
asahi-fwextract does locally on an installed machine.

Usage:
  fetch_firmware.py --list                  # show DeviceTree + AVE members
  fetch_firmware.py --board j314c --variant H13C
"""
import argparse, sys, time, json, urllib.request

CATALOG = "https://api.ipsw.me/v4/device/{model}?type=ipsw"

def latest_ipsw(model):
    with urllib.request.urlopen(CATALOG.format(model=model), timeout=30) as r:
        d = json.load(r)
    fw = d["firmwares"][0]
    return fw["url"], fw["version"], fw["buildid"]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="MacBookPro18,3",
                    help="Mac identifier used to locate the IPSW (default: MacBookPro18,3)")
    ap.add_argument("--board", default="j314c", help="ADT board id, e.g. j314c")
    ap.add_argument("--variant", default="H13C", help="AVE firmware variant, e.g. H13C")
    ap.add_argument("--outdir", default="data/blobs")
    ap.add_argument("--list", action="store_true", help="only list relevant members")
    ap.add_argument("--url", help="explicit IPSW URL (skips the catalog lookup)")
    a = ap.parse_args()

    try:
        from remotezip import RemoteZip
    except ImportError:
        sys.exit("pip install -r requirements.txt first")

    url = a.url
    if not url:
        url, ver, build = latest_ipsw(a.model)
        print(f"IPSW {a.model}: {ver} ({build})\n  {url}")

    wanted = [f"Firmware/all_flash/DeviceTree.{a.board}ap.im4p",
              f"Firmware/ave/AppleAVE2FW_{a.variant}.im4p"]

    # Apple's CDN resets long-lived range sessions; retry each member.
    for name in wanted:
        for attempt in range(5):
            try:
                with RemoteZip(url) as z:
                    if a.list:
                        for n in z.namelist():
                            if "DeviceTree" in n or "/ave/" in n:
                                print(f"  {z.getinfo(n).file_size:>9}  {n}")
                        return
                    z.extract(name, a.outdir)
                    print(f"  extracted {name}")
                break
            except Exception as e:
                print(f"  attempt {attempt+1} for {name} failed: {type(e).__name__}")
                time.sleep(4)
        else:
            sys.exit(f"could not fetch {name}")
    print(f"\nNow unwrap the Image4 containers:\n"
          f"  pyimg4 im4p extract -i {a.outdir}/.../<file>.im4p -o <file>.bin")

if __name__ == "__main__":
    main()
