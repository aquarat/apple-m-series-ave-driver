# bench/hevc-efficiency

The HEVC compression benchmark behind docs/88 (AVE vs x265). Results and
conclusions are in docs/88; this is how to rerun it.

| file | |
|---|---|
| `prepare.sh` | fetches frames 0-199 of the five Xiph derf 1080p50 clips and converts them to raw I420 + NV12 |
| `bench.py` | `run <clips>`: encode, decode, score (PSNR, SSIM, VMAF), append to `results.csv`. `report()`: PCHIP BD-rate tables |
| `power.py` | energy per frame (SMC "Total System Power"; Apple silicon, macsmc-hwmon) |
| `results.csv` | every run from 2026-09-30 (315 rows): clip, encoder, target (kbit/s, QP or CRF), kbit/s, wall/CPU s, fps, PSNR-Y, PSNR avg, SSIM, VMAF |
| `power.log` | the energy run |
| `x265-4.1-cmake4.patch` | x265 4.1 sets two CMake policies to OLD, which CMake 4 refuses; this sets them NEW |

## Rerun

Needs: the apple-ave driver loaded (encoder node `apple-ave-enc`); an
ffmpeg with `hevc_v4l2m2m` (`FF_ENC`) and one with an HEVC decoder
(`FF_DEC`; Fedora's ffmpeg has no HEVC decoder); `v4l2-ctl`; x265 4.1
built twice (8-bit and `-DHIGH_BIT_DEPTH=ON`); libvmaf 3.0.0's `vmaf`
tool. About 10 GB of scratch space in `BENCH_WORK`.

```sh
export BENCH_WORK=/scratch/hevc FF_DEC=/path/to/ffmpeg-with-hevc-decoder
bench/hevc-efficiency/prepare.sh
git clone --depth 1 --branch 4.1 https://bitbucket.org/multicoreware/x265_git.git $BENCH_WORK/src/x265
git -C $BENCH_WORK/src/x265 apply $PWD/bench/hevc-efficiency/x265-4.1-cmake4.patch
for b in arm:OFF 10:ON; do d=${b%%:*}; mkdir -p $BENCH_WORK/src/x265/build-$d && (cd $BENCH_WORK/src/x265/build-$d &&
  cmake -G Ninja ../source -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DHIGH_BIT_DEPTH=${b#*:} \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && ninja); done
git clone --depth 1 --branch v3.0.0 https://github.com/Netflix/vmaf.git $BENCH_WORK/src/vmaf
(cd $BENCH_WORK/src/vmaf/libvmaf && meson setup build --buildtype release -Dbuilt_in_models=false && ninja -C build)

cd bench/hevc-efficiency
RESULTS=my-results.csv python3 bench.py run crowd_run park_joy ducks_take_off in_to_tree old_town_cross
# ENCODERS=ave-cqp,x265-medium-crf,... limits the encoder list (names: docs/88 §3)
RESULTS=my-results.csv python3 -c "import bench; bench.report([('ave-cqp','x265-medium-crf'), ('ave-cqp','x265-slow-crf')],
  groups={'xiph':['crowd_run','park_joy','ducks_take_off','in_to_tree','old_town_cross']})"
```

Everything runs from raw files; nothing is written outside `BENCH_WORK`
and the results file. Software encodes run at `nice 10`.
