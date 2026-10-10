# 99. Firmware placement from the live ADT, and packaging without per-machine data

Status 2026-10-10: **implemented and compiled; not yet run on hardware.**
The parser passes its userspace self-test; the probe path needs the runs in
§5.

## 1. The problem

Every SoC row in `driver/ave_soc.c` carries the physical addresses where
iBoot put the encoder's 13.5 firmware (TEXT, and DATA where iBoot loads
it) on the machine that port was brought up on. iBoot decides those
addresses per boot chain, so a row is one machine's observation:

| row | from |
|---|---|
| t6001 | the lab M1 Max (docs/44 §2.4) |
| t6000 | one M1 Pro, a 16 MiB dump and a search (docs/87 §3) |
| t8103 | a j313's m1n1 proxy session; a j274 read the same (docs/89) |
| t8112 | one j473, three carve-outs (docs/90 §3) |
| t6002 | one Mac Studio, from its live ADT (docs/98 U0) |

The driver refuses a wrong TEXT at stage 10 (RVBAR check), but a wrong
in-place DATA address with the right TEXT would hand the core foreign
memory (docs/98 §4). That is why t6002 was gated behind a per-machine U0
step, and why the packaging marked it "one machine's placement".

## 2. Where the answer is

macOS does not carry addresses either. Its kext reads `pre-loaded` and
`segment-ranges` off the encoder's node in the live ADT (docs/09 §1.1-1.2):
32-byte entries `{u64 phys, u64 iova, u64 remap, u32 size, u32 flags}`,
TEXT at iova 0 and DATA at the iova that follows it.

Since v1.6.1, m1n1 publishes the ADT iBoot handed it as a reserved-memory
node, `compatible = "phram"`, `label = "adt"` (docs/98 §4). The lab M1 Max
has it (`flash@10004984000` under `/reserved-memory`, m1n1 v1.6.1), as do
the M1 Pro and the M1 Ultra. It is DRAM Linux never writes; reading it
touches no register.

## 3. What the driver does

`ave_fw_placement_from_adt()` (`driver/ave_fw.c`, parser in
`driver/ave_adt.c`) runs at probe, right after the row is picked and
before any register access:

1. find the reserved-memory node (`phram`, label `adt`) and map it
   cacheable (at most 16 MiB);
2. find the `/arm-io` child named `ave` or `ave<N>` whose `reg[0]`,
   translated through `/arm-io`'s `ranges`, is the row's DPE bank. The node
   is matched by address, not by name;
3. if it has `pre-loaded` and a TEXT segment of the row's TEXT size, take
   its address; take the DATA segment of the row's DATA size for a row
   whose DATA is in place (iBoot's) or decided at probe (t8103). A row with
   driver-owned DATA (t6001's ave1) keeps its own: the ADT's DATA is not
   used for it;
4. the same placement as the row: logged, nothing changes. Another
   placement: the driver uses the ADT's in a per-device copy of the row
   (`iboot_adt=1`, default), or only logs it (`iboot_adt=2`).
   `iboot_adt=0` skips the ADT;
5. no ADT, no matching node, no `pre-loaded`, a segment of another size,
   or a malformed ADT: the compiled row stands, with a log line saying why.

Everything after that is unchanged and still checks the image: RVBAR
against TEXT, the DATA literal at TEXT+0x423c, the TEXT identity windows,
and DATA outside System RAM and every other reservation.

## 4. Offline evidence

`tools/adt_selftest` (`make -C tools/adt_selftest check`, ASan and UBSan):
a synthetic live ADT with a decoy node (`ave-hint`, same reg), a
pre-loaded `ave0` with TEXT and DATA, an `ave1` without segments, and an
`ave3` outside every range; near-miss and untranslatable addresses must
not match; every truncation and 20 000 random single-byte corruptions
must be refused or answered without an out-of-bounds read; and the j314c
restore ADT must yield ave0 at 0x40d100000 and ave1 at 0x507100000, neither
pre-loaded (no iBoot ran on a restore ADT). 1273 checks, 0 failed.
The module builds with `W=1` and no warnings.

## 5. Hardware runs needed (docs/53, d1-d5)

On the M1 Max, each from a fresh boot, one load per boot:

- **d1** defaults (`iboot_adt=1`, and `dapf_by_driver` now set on t6001's
  ave0): the log must read `adt: /arm-io/ave0: pre-loaded yes, 2
  segment(s); TEXT 0x10000b28000, DATA 0x10001a90000` and `placement
  matches ave_soc.c's t6001 row`; ave1 must keep its owned DATA; both
  encoders encode 720p H.264 at the known PSNR. A "no": any other
  placement, a REFUSING line, a hang, or a PSNR change.
- **d2** = d1 again (repeat before believing).
- **d3** `iboot_adt=0`: byte-identical streams to d1 (the control).
- **d4** on stock m1n1, the DAPF programmed by the driver alone: needs an
  m1n1 swap on the lab machine, so the operator decides.
- **d5** the soak (`tools/stress.sh`) and `v4l2-compliance` on this build.

Other SoCs: the first load on each with this build should log `placement
matches` (t6000, t8112, t8103 j274, t6002 on the machine its block came
from). The first M1 Ultra other than that one is the case this exists
for: its log should show `using the ADT's`, and U2's 720p check should
pass without a U0 step.

## 6. What the ADT does not give: the pristine DATA blob

The blob is only needed to restart a core in the same boot (a reload or a
hang recovery) and for driver-owned DATA. `tools/data_blob_from_image.py`
builds it from the image for H13S, H13C and H14G byte for byte, and for
H13G with `--tunables none` (accepted for owned DATA only, docs/89 §6).
H13D's fills (`SOC_`, `SOCR`, tunables) were never transcribed, and the
four pinned t6002 blobs are one machine's dumps. On another M1 Ultra the
first load of a boot works without a blob (all four encoders are
preloaded, docs/98 §11). A reload in the same boot cannot reset and
restore the core without a blob, so it fails at probe (the halted core
does not start again, docs/84 §4; another machine's blob is refused at
the pinned sha256 check): reboot instead. The fix for that is to
transcribe H13D's fills from one t6002 DATA dump, as §51's 2026-10-10
note did for H13C.
