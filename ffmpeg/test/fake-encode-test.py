#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run ffmpeg's v4l2m2m encoders against fake-ave-v4l2.so and check the output.

No hardware is touched: the fake device lives at a path that does not exist,
ffmpeg is always given that path with -device (so it never scans /dev/video*),
and LD_PRELOAD routes that path to the emulation.

  fake-encode-test.py --ffmpeg BUILD/ffmpeg --ffprobe BUILD/ffprobe \
      --fake fake-ave-v4l2.so --template-ffmpeg /usr/bin/ffmpeg [--work DIR] [-k PATTERN]

--template-ffmpeg must have a software H.264 encoder (libopenh264 or libx264):
it makes the template streams the fake device copies its packets from.

Checks, per case:
  ts      packet count, the PTS set (every input frame once), DTS strictly
          rising and never above the PTS, the first packet a key frame,
          extradata present (Matroska, MP4), B frames where asked for
  picture what the device received (FAKE_AVE_DUMP, cropped) is byte-identical
          to ffmpeg's own conversion of the source (stride, chroma offset,
          crop, yuv420p->NV12 and yuv420p10->P010 conversion)
  ctrl    what the device was told (QP, rate control, B frames, crop)
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

FPS = 25


def run(cmd, env=None, timeout=120):
    p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=timeout)
    return p.returncode, p.stdout, p.stderr.decode(errors="replace")


class Runner:
    def __init__(self, a):
        self.a = a
        self.work = a.work or tempfile.mkdtemp(prefix="fake-ave-")
        os.makedirs(self.work, exist_ok=True)
        self.dev = os.path.join(self.work, "fake-video-node")  # never created
        self.templates = {}
        self.failed = []
        self.passed = 0

    def template(self, w, h):
        key = (w, h)
        if key not in self.templates:
            path = os.path.join(self.work, f"tmpl-{w}x{h}.h264")
            enc = "libx264" if self.has_encoder("libx264") else "libopenh264"
            rc, _, err = run([self.a.template_ffmpeg, "-v", "error", "-y", "-f", "lavfi",
                              "-i", f"testsrc2=s={w}x{h}:r={FPS}", "-frames:v", "2",
                              "-c:v", enc, "-bf", "0", "-f", "h264", path])
            if rc:
                sys.exit(f"cannot make a template with {self.a.template_ffmpeg}: {err}")
            self.templates[key] = path
        return self.templates[key]

    def has_encoder(self, name):
        rc, out, _ = run([self.a.template_ffmpeg, "-hide_banner", "-encoders"])
        return name.encode() in out

    def env(self, w, h, **extra):
        e = dict(os.environ)
        e.update(FAKE_AVE_PATH=self.dev, FAKE_AVE_TEMPLATE=self.template(w, h),
                 LD_PRELOAD=os.path.abspath(self.a.fake), FAKE_AVE_LOG="1")
        e.update({k: str(v) for k, v in extra.items()})
        return e

    def check(self, name, ok, why=""):
        if ok:
            self.passed += 1
        else:
            self.failed.append(f"{name}: {why}")
            print(f"FAIL {name}: {why}", flush=True)

    def encode(self, name, w, h, frames, enc_args, out, fmt=None, src_fmt="yuv420p",
               vf=None, codec="h264_v4l2m2m", fake_env=None, extra_in=None):
        """Encode testsrc2 through the fake device; return (rc, stderr)."""
        src = f"testsrc2=s={w}x{h}:r={FPS}"
        cmd = [self.a.ffmpeg, "-hide_banner", "-nostdin", "-y", "-f", "lavfi", "-i", src]
        if extra_in:
            cmd += extra_in
        cmd += ["-frames:v", str(frames)]
        if vf:
            cmd += ["-vf", vf]
        cmd += ["-pix_fmt", src_fmt, "-c:v", codec, "-device", self.dev] + enc_args
        if fmt:
            cmd += ["-f", fmt]
        cmd.append(out)
        rc, _, err = run(cmd, env=self.env(w, h, **(fake_env or {})))
        if rc:
            tail = "\n".join(l for l in err.splitlines() if "[fake-ave] coded" not in l)[-1500:]
            self.check(name, False, f"ffmpeg exit {rc}\n{tail}")
        return rc, err

    def probe(self, path):
        rc, out, err = run([self.a.ffprobe, "-v", "error", "-select_streams", "v:0",
                            "-show_packets", "-show_streams", "-show_entries",
                            "packet=pts,dts,flags:stream=extradata_size,codec_name,width,height",
                            "-of", "json", path])
        if rc:
            return None
        return json.loads(out)

    # ---- timestamps and containers ----------------------------------------
    def case_ts(self, bf, gop, container, shuffle=0, eos_early=0, frames=60, qp=24,
                vf=None, enc_tb=None):
        name = f"ts bf={bf} g={gop} {container} n={frames} shuffle={shuffle} eos_early={eos_early}" + \
               (f" vf={vf}" if vf else "")
        if self.a.k and self.a.k not in name:
            return
        out = os.path.join(self.work, f"ts-{bf}-{gop}-{shuffle}-{eos_early}.{container}")
        args = ["-bf", str(bf), "-g", str(gop)]
        if qp is not None:
            args += ["-qp", str(qp)]
        if enc_tb:
            args += ["-enc_time_base", enc_tb, "-fps_mode", "passthrough"]
        rc, err = self.encode(name, 720, 576, frames, args, out, vf=vf,
                              fake_env=dict(FAKE_AVE_SHUFFLE=shuffle, FAKE_AVE_EOS_EARLY=eos_early))
        if rc:
            return
        j = self.probe(out)
        if not j:
            self.check(name, False, "ffprobe failed")
            return
        pk = j["packets"]
        st = j["streams"][0]
        self.check(name + " count", len(pk) == frames, f"{len(pk)} packets for {frames} frames")
        pts = [int(p["pts"]) for p in pk if p.get("pts") not in (None, "N/A")]
        self.check(name + " pts", len(pts) == len(pk) and len(set(pts)) == len(pts),
                   "missing or repeated PTS")
        if not vf:
            step = (sorted(pts)[1] - sorted(pts)[0]) if len(pts) > 1 else 1
            self.check(name + " pts set", sorted(pts) == [sorted(pts)[0] + i * step
                                                           for i in range(len(pts))],
                       "the PTS are not every frame once")
        if container in ("mp4", "ts", "nut"):
            dts = [int(p["dts"]) for p in pk]
            bad = [i for i in range(len(pk)) if dts[i] > int(pk[i]["pts"]) or
                   (i and dts[i] <= dts[i - 1])]
            self.check(name + " dts", not bad,
                       f"DTS > PTS or not rising at packets {bad[:5]}: " +
                       ", ".join(f"{p['pts']}/{p['dts']}" for p in pk[:8]))
        self.check(name + " key", pk and pk[0]["flags"].startswith("K"), "first packet not key")
        reordered = any(pts[i] < pts[i - 1] for i in range(1, len(pts)))
        latched = bf if not gop or gop >= bf + 2 else max(gop - 2, 0)
        if frames < 3:      # an IDR, then at most a P: nothing to reorder
            latched = 0
        self.check(name + " reorder", reordered == (latched > 0),
                   f"reordering {reordered}, expected B frames {latched}")
        if container in ("mkv", "mp4"):
            self.check(name + " extradata", int(st.get("extradata_size", 0)) > 0,
                       "no extradata")
        m = re.findall(r"close: (\d+) source frames coded in (\d+) session", err)
        sessions = int(m[-1][1]) if m else -1
        want = 2 if container in ("mkv", "mp4") else 1
        self.check(name + " sessions", sessions == want,
                   f"{sessions} sessions (want {want}: the global header probe restarts)")

    # ---- what the device received -----------------------------------------
    def case_picture(self, w, h, src_fmt, codec="h264_v4l2m2m", raw="h264", frames=5):
        name = f"picture {w}x{h} {src_fmt} {codec}"
        if self.a.k and self.a.k not in name:
            return
        dump = os.path.join(self.work, f"dump-{w}x{h}-{src_fmt}.yuv")
        ref = os.path.join(self.work, f"ref-{w}x{h}-{src_fmt}.yuv")
        out = os.path.join(self.work, f"pic-{w}x{h}-{src_fmt}.{raw}")
        for p in (dump, ref):
            if os.path.exists(p):
                os.unlink(p)
        rc, err = self.encode(name, w, h, frames, ["-qp", "20"], out, fmt=raw,
                              src_fmt=src_fmt, codec=codec, fake_env=dict(FAKE_AVE_DUMP=dump))
        if rc:
            return
        dev_fmt = "p010le" if "10" in src_fmt else "nv12"
        rc, _, e2 = run([self.a.ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
                         f"testsrc2=s={w}x{h}:r={FPS}", "-frames:v", str(frames),
                         "-pix_fmt", src_fmt, "-f", "rawvideo", "-pix_fmt", dev_fmt, ref])
        if rc:
            self.check(name, False, "reference: " + e2)
            return
        a = open(dump, "rb").read() if os.path.exists(dump) else b""
        b = open(ref, "rb").read()
        first = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), None)
        self.check(name, a == b,
                   f"dump {len(a)} bytes, reference {len(b)}, first difference at {first}")
        cw, ch = (w + 15) // 16 * 16, (h + 15) // 16 * 16
        if (cw, ch) != (w, h):
            self.check(name + " crop", f"crop {w}x{h} of {cw}x{ch}" in err,
                       "no crop: " + "\n".join(l for l in err.splitlines() if "crop" in l))

    # ---- what the device was told ---------------------------------------------
    def case_ctrl(self, name, args, want):
        if self.a.k and self.a.k not in name:
            return
        out = os.path.join(self.work, "ctrl.h264")
        rc, err = self.encode("ctrl " + name, 720, 576, 4, args, out, fmt="h264")
        if rc:
            return
        m = re.search(r"session 1: .*", err)
        line = m.group(0) if m else ""
        self.check("ctrl " + name, want in line, f"want '{want}' in '{line}'")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ffmpeg", required=True)
    ap.add_argument("--ffprobe", required=True)
    ap.add_argument("--fake", required=True, help="fake-ave-v4l2.so")
    ap.add_argument("--template-ffmpeg", default="ffmpeg",
                    help="an ffmpeg with libx264 or libopenh264 (default: ffmpeg)")
    ap.add_argument("--work", help="work directory (default: a new temporary one)")
    ap.add_argument("-k", help="run only the cases whose name contains this")
    r = Runner(ap.parse_args())

    for container in ("mkv", "mp4", "ts"):
        for bf in (0, 1, 2):
            for gop in (250, 12):
                r.case_ts(bf, gop, container)
    for bf in (1, 2):
        r.case_ts(bf, 250, "mp4", shuffle=1)
        r.case_ts(bf, 250, "mkv", eos_early=1)
        r.case_ts(bf, 250, "mp4", eos_early=1, shuffle=1)
        r.case_ts(bf, 3, "mp4")                 # the driver keeps one B (GOP 3)
        r.case_ts(bf, 250, "mp4", frames=1)
        r.case_ts(bf, 250, "mp4", frames=2)
        r.case_ts(bf, 250, "ts", frames=7)
        r.case_ts(bf, 250, "mp4", qp=None)      # rate control
    # variable frame rate
    r.case_ts(2, 250, "mp4", vf="settb=1/1000,setpts=N*40+mod(N\\,3)*7", enc_tb="-1")

    r.case_picture(720, 576, "yuv420p")
    r.case_picture(720, 576, "nv12")
    r.case_picture(1920, 1080, "yuv420p")
    r.case_picture(1916, 1078, "nv12")
    r.case_picture(704, 480, "yuv420p")
    r.case_picture(720, 576, "yuv420p10le", codec="hevc_v4l2m2m", raw="hevc")
    r.case_picture(1920, 1080, "yuv420p10le", codec="hevc_v4l2m2m", raw="hevc")

    r.case_ctrl("qp", ["-qp", "24", "-bf", "1"], "B 1, GOP 12, RC 0 mode 2, QP 24/24/27")
    r.case_ctrl("qp offsets", ["-qp", "30", "-qp_p_offset", "2", "-qp_b_offset", "4"],
                "RC 0 mode 2, QP 30/32/34")
    r.case_ctrl("q:v", ["-q:v", "20"], "RC 0 mode 2, QP 20/20/23")
    r.case_ctrl("rate control", ["-b:v", "3M"], "RC 1 mode 0")
    r.case_ctrl("bf clamp", ["-qp", "24", "-bf", "5", "-g", "250"], "B 2, GOP 250")

    print(f"{r.passed} checks passed, {len(r.failed)} failed (work directory {r.work})")
    for f in r.failed:
        print("  " + f.splitlines()[0])
    sys.exit(1 if r.failed else 0)


if __name__ == "__main__":
    main()
