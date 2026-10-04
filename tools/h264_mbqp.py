#!/usr/bin/env python3
"""h264_mbqp.py IN.h264 [FFMPEG]: per-frame macroblock QPs, the output of tools/h264_mbqp.c
(`n type qp mean min max`, qp = the most common MB QP), from `ffmpeg -debug qp`. For an
FFmpeg whose libraries cannot be linked against; needs FFmpeg's native h264 decoder."""
import collections, re, subprocess, sys

ff = sys.argv[2] if len(sys.argv) > 2 else "ffmpeg"
err = subprocess.run([ff, "-hide_banner", "-threads", "1", "-debug", "qp", "-i", sys.argv[1], "-f", "null", "-"],
                     capture_output=True, text=True).stderr
frames, cur = [], None
for line in err.splitlines():
    m = re.search(r"New frame, type: (\w)", line)
    if m:
        cur = [m[1], []]
        frames.append(cur)
        continue
    m = re.match(r"\[h264 @ [^]]*\]\s+\d+ (\d+)$", line)
    if cur and m:
        d = m[1]
        cur[1] += [int(d[i:i + 2]) for i in range(0, len(d) - 1, 2)]
# avformat's stream probing decodes the first frames once more, before the real decode
fp = ff[:-len("ffmpeg")] + "ffprobe" if ff.endswith("ffmpeg") else "ffprobe"
npk = int(subprocess.run([fp, "-v", "error", "-count_packets", "-show_entries", "stream=nb_read_packets",
                          "-of", "csv=p=0", sys.argv[1]], capture_output=True, text=True).stdout.split()[0])
frames = frames[-npk:]
for n, (t, q) in enumerate(frames):
    if not q:
        print(f"{n} -")
        continue
    print(f"{n} {t} {collections.Counter(q).most_common(1)[0][0]} {sum(q) / len(q):.3f} {min(q)} {max(q)}")
