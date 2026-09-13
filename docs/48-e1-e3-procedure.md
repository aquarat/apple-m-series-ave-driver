# E1-E3 operator procedure: ISP peek, AVE DAPF dump, DAPF programming

Written 2026-09-13. **None of this has been run.** The code is compiled only.
This is the operator procedure for the experiments proposed in
[44-reset-fetch-path.md](44-reset-fetch-path.md) §6. Read 44,
[24-incident-2026-09-07.md](24-incident-2026-09-07.md) and
[00-methodology.md](00-methodology.md) first. Agents must not run any of it
([AGENTS.md](../AGENTS.md)).

Code:

| file | what |
|---|---|
| `test/isp_peek.c` | E1, a standalone read-only module |
| `driver/ave_dapf.c`, `driver/ave_dapf.h` | E2 dump, E3 DAPF programming |
| `driver/ave_fw.c` | E3 DART mappings for iBoot's DATA/TEXT (`fw_map_data`, `fw_map_text`) |
| `test/ave-overlay-e2.dts` | overlay `variant=2`: variant 1 + `cpudart`/`dapf` reg |
| `test/ave-overlay-e3.dts` | overlay `variant=3`: variant 0 + `cpudart`/`dapf` reg |

Every new parameter defaults to off. A default `insmod` behaves exactly as
before these changes.

---

## 0. Rules common to all three

- **Nothing is installed.** Modules are `insmod`-ed from the tree. The
  overlay is RAM-only. Nothing is written to `/boot`, the ESP, `/etc` or
  `/lib/modules`. A reboot clears every effect. **No step touches m1n1 or boot
  configuration**, so no step can make the machine unbootable. The residual
  risk from a hard hang is filesystem damage: `sync` before every load.
- **Crash visibly.** Set up netconsole to another host before E2/E3
  ([24](24-incident-2026-09-07.md)). Tee `dmesg -w` into `results/` and
  `sync`, as `tools/handshake-test.sh` does.
- **One overlay variant per boot.** `test/ave-overlay.ko` has no
  `module_exit`, so a variant cannot be removed or swapped. A different
  variant needs a reboot.
- **Refuse blind runs.** If `dmesg | grep "Disabling IRQ #"` matches, the
  DART fault line is dead and E2/E3 results cannot be read. Reboot.
- **Build:**

  ```sh
  make -C test          # ave-overlay.ko (4 variants), isp_peek.ko, physdump.ko
  make -C driver        # needs ave_dapf.o in driver/Makefile (integrator)
  modinfo -p driver/apple-ave.ko | grep -E 'dapf|fw_map'   # prove the build has them
  ```

- **Recovery from a hang:** there is no console. Hold power ~10 s. The PMU
  resets the SoC. Nothing needs undoing afterwards. Check with
  `journalctl -b -1 | tail` and the tee'd log.

---

## E1 - ISP RVBAR, CPU state and DAPF, read-only

**Question:** is a physical RVBAR normal for an ASC that works on Linux, and
does the t8020 DAPF register layout read back what m1n1 programmed?

**Needs a fresh boot?** No. It does not touch AVE. It can run on a boot
where the AVE overlay is loaded or an IRQ was disabled.

### Procedure

```sh
# terminal 1: start the camera and leave it running for the whole step
ffmpeg -f v4l2 -i /dev/video0 -f null -

# terminal 2
cat /sys/bus/platform/devices/384000000.isp/power/runtime_status   # must say: active
sudo cat /sys/kernel/debug/pm_genpd/pm_genpd_summary   # every domain listing 384000000.isp or genpd:*:384000000.isp: on
sync; sync
sudo insmod test/isp_peek.ko             # optionally: dart_regs=1
sudo dmesg | grep isp_peek | tee results/e1-$(date +%s).log
cat /sys/bus/platform/devices/384000000.isp/power/runtime_status   # still active?
sudo rmmod isp_peek                      # holds nothing; exit is empty
```

Stop `ffmpeg` only after `insmod` has returned.

### What the module checks before it reads anything

1. The `apple,isp` node's `reg[0]` is `0x384000000`. Its first `iommus` DART
   is `0x3860e8000`. Otherwise it refuses.
2. It takes `device_lock` with trylock and refuses if the lock is busy. The
   ISP device must be bound and `pm_runtime_active()`.
3. It calls `pm_runtime_get_if_active()` on the ISP device, **on every
   `genpd:*` supplier**, and on the DART. The number of genpd devices pinned
   must equal the node's `power-domains` count (10). The genpd pins matter
   because apple-isp powers domains 1..9 separately, and its shutdown path
   drops them *before* it puts the ISP device.
4. Only then does it `ioremap` and read each register once. Afterwards it
   drops every reference.

### Expected output and what it means

Negative control: `CPU_STATUS` must show `RUNNING`. If it does not, the
module prints `NEGATIVE CONTROL FAILED` and the RVBAR line is not evidence.

| `RVBAR base` | reading |
|---|---|
| in `0x1f000000000`-`0x1f0ffffffff` | ISP boots TEXT through the DART window. AVE's physical TEXT fetch is AVE-specific. The E3 TEXT entry is still the right test for AVE. |
| equals `ISP TEXT phys` (printed, from the `apple,asc-mem` node at ISP IOVA 0; `0x10000c68000` this boot) | ISP also fetches TEXT physically. Look for the DAPF line marked `<- admits RVBAR base`. If present, it supports H1's mechanism and gives the r0/r4 a TEXT entry needs. If absent, check `dart_regs=1`: `BYPASS_DAPF` on ISP's stream means the filter is not enforced; otherwise H1's "DAPF must admit TEXT" is in trouble. |
| below 4 GiB | IOVA-looking. Neither window. Doc 44 §0 is incomplete for ISP. |
| other | unexplained; record it |

DAPF slots are compared slot for slot with the 13.5 ADT's `dart-isp0` list
(15 entries, compiled in):

- *(As run, E1 gave 0/15 slot-for-slot because of the injected TEXT slot and
  masked ends — see Results. The layout test that matters is that every slot
  decodes to a sane range, which it did.)*
- **15/15 MATCH, slot 15 `no ADT entry` and empty:** the layout `ave_dapf.c`
  uses is right, and m1n1's programming survived ISP power cycles, which is
  evidence against H3 for ISP. This is the positive control E3 depends on.
- **Slot 15 non-empty, or all DIFF, or all zero:** the layout, the address or
  the survival assumption is wrong. **Do not run E3** until this is explained.
- A second negative control, which reads nothing: run the `insmod` with the
  camera stopped. It must refuse with `not runtime-active`. If it does not
  refuse, the power check is broken; do not run it with the camera on either.

### Abort criteria

- The `insmod` refuses for any reason other than the deliberate stopped-camera
  control: stop, and fix the precondition. Do not work around it.
- `runtime_status` is not `active` at the start: do not load.

### Risk

**Low.** The reads are of another driver's MMIO, with every one of its power
domains pinned for the duration. The remaining hazard is a stale device-link
walk if the ISP driver is unbound concurrently. Do not unbind or rmmod
apple-isp during the step. Worst case is a fabric hang and a reboot.
**Bootability:** unaffected; nothing persists.

---

## E2 - AVE CPUDART and DAPF state, read-only

**Question:** what DAPF entries, TCR/TTBR and REMAP does the AVE CPUDART
hold before Linux's DART driver has touched it? Is there already an entry
admitting TEXT? Is `DAPF_LOCK` set?

**Needs a fresh boot:** yes. The overlay is once per boot, and apple-dart
must never have bound.

### Procedure (E2, variant=2)

```sh
# fresh boot, netconsole up, dmesg -w tee'd to results/
sudo dmesg | grep -q "Disabling IRQ #" && echo "REBOOT FIRST"
sync; sync
sudo insmod test/ave-overlay.ko variant=2
ls -d /sys/bus/platform/devices/*video-encoder*      # present
ls /sys/bus/platform/devices/40d100000.video-encoder/iommu_group 2>/dev/null && echo "WRONG VARIANT - has an IOMMU"
sync; sync; sleep 5
sudo insmod driver/apple-ave.ko stop_after=8 dapf_dump=1
sudo dmesg | grep -a 'apple-ave\|dapf' | tee results/e2-v2-$(date +%s).log
sudo rmmod apple_ave        # powers VENC off
```

**Do not go past `stop_after=8`.** Without an IOMMU, stage 9 would hand the
coprocessor unprotected DMA.

### Control (E2c, variant=3), on a different fresh boot

```sh
sudo insmod test/ave-overlay.ko variant=3
cat /sys/kernel/iommu_groups/*/type    # the video-encoder group must be DMA
sudo insmod driver/apple-ave.ko stop_after=8 dapf_dump=1
```

This dump runs after apple-dart's reset. Expect TCR[0] `TRANSLATE`, valid
TTBR[0][0], and a DAPF **identical** to the variant=2 dump, because Linux
does not touch the DAPF. If the DAPF differs between the two, something
other than apple-dart writes it, or it does not survive gating consistently.
Record both.

### What the dump checks before reading

- It requires `cpudart` = `0x40d040000`, `dapf` = `0x40d044000`, and
  `dapf` = `cpudart + 0x4000`. With `iommus` present, the DART's `reg` must
  equal `cpudart`. Otherwise it refuses.
- It requires the stage 6 runtime-PM reference to be held (`ave->powered`,
  `pm_runtime_active`). Otherwise it refuses.
- It uses `devm_ioremap`, never request. The reads are PARAMS1/2, ERROR,
  ERROR_ADDR, CONFIG, REMAP[0-3], DAPF_LOCK, 0xf8, ENABLED_STREAMS,
  TCR/TTBR[0..3] for SIDs 0, 1 and 15, and 16 DAPF entries × {r0, r4, start,
  end}.

### Reading it

| observation | meaning |
|---|---|
| all 16 DAPF slots `empty` | Nothing admits anything: gating wiped iBoot's filter, or iBoot never set one. E3 must program all entries. Consistent with H1+H3. |
| a slot `admits:0x10000b28200` | iBoot's TEXT entry survived gating. **H3 refuted** as stated. Then ask why the fetch still faults; compare TCR (`BYPASS_DAPF`?) and the E2c dump. |
| slots matching the ADT (0x1f0 window, an MMIO span) but no TEXT slot | The ADT filter is programmed and TEXT is missing, which is exactly H1's gap. Note **which** MMIO entry is present: that answers the cross-wiring question for ave0. |
| `DAPF_LOCK ... LOCKED` | E3 cannot work. `ave_dapf_program()` refuses on it. |
| `CONFIG LOCKED` | DART locked by iBoot. apple-dart would skip its reset (doc 44 §1.2). Record it. |
| TTBR VALID on variant=2 | iBoot DART state survives gating. That bears on H3 and on doc 41. |
| `REMAP[0]` byte 1 (MAP1, SID 1's target) | `0` is consistent with the ADT's `remap = 1` (SID 1 → 0, doc 44 §2.3), but is also what an all-zero reset reads. `1` means identity: nothing remapped. Read it against bytes 0, 2 and 3. |

### Abort criteria

- It refuses on addresses: the overlay build is wrong. Rebuild `test/` and
  decode the dtbo (`dtc -I dtb -O dts test/ave-overlay-e2.dtbo`). Do not edit
  addresses to make it pass.
- Stage 6 fails or genpd shows `venc_*` off: stop. Do not retry until it is
  understood.

### Risk

**Low to moderate.** These are the first reads of AVE's CPUDART
`0x000-0x2fc` and its DAPF `0x000-0x3ff` from Linux. The DAPF read has never
been done on AVE. Every read is gated on venc_sys held on, and the offsets
are ones m1n1 and apple-dart define. The core is not started. On variant=2 no
driver handles DART IRQ 1028; reads do not assert it. Worst case: a fabric
hang, then a reboot. **Bootability:** unaffected; nothing persists.

---

## E3 - Admit iBoot's firmware through the DAPF, keep translation

**Only if** E1 decoded every slot to a sane range (layout confirmed — it did:
the live list is the ADT's shifted by one injected TEXT slot, with ends masked,
so a slot-for-slot match was never going to happen) **and** E2 showed
`DAPF_LOCK` clear (it did).

**Needs a fresh boot:** yes (variant=3). The video-encoder group must be `DMA`
(the boot default; do not set it to identity).

### Parameters

| parameter | default | values |
|---|---|---|
| `dapf_set` | `off` | `control` = 0x1f0 window + MMIO, **no TEXT** (negative control); `text` = window + MMIO + TEXT |
| `dapf_mmio` | `ave0` | `ave0` = `0x40d050000-0x40dc69000` (ave0's own span, listed under dart-ave1); `adt` = `0x506000000-0x507c6c000` (as dart-ave0 lists it; ave1's span); `both`; `none` |
| `fw_map_data` | 0 | 1 = DART map DVA `0xec000` → phys `0x10001a90000`, `0x134000`, RW |
| `fw_map_text` | 0 | 0 = legacy: our image at DVA `0xb28000`; 1 = iBoot TEXT phys `0x10000b28000` at DVA `0xb28000`, read-only; 2 = nothing at DVA `0xb28000` |
| `dapf_dump` | 0 | 1 = also dump at stage 8 and after the run |

The entries are written exactly as `dapf_init_t8020()` writes them: `+4 r4`,
`+8 start`, `+0x10 end`, then `+0 r0`, at `+0x40` per slot. They are then read
back and compared. The window and MMIO entries go in ADT order, and TEXT is
appended as slot `n-1`.

- *(Superseded by E1 — see "Results" below: the TEXT entry is now `0x10000b28000`-`0x10000c13ffc`, `r0 0x11`.)* TEXT entry = `0x10000b28000`-`0x10000c13fff`, `r0 0x33`, `r4 1`, copying
  the window entry.
- **`end` is inclusive.** `dart-isp0`'s ADT list has `0x28ec3c000-0x28ec3c003`
  and `0x285460000-0x285460003`, which are single 32-bit registers, and m1n1
  writes ADT values unmodified.

Checks before any DAPF write:

- a translating (paging) domain;
- TCR[0] has `TRANSLATE` and no `BYPASS_*`;
- stage 6 reference held;
- address checks as in E2;
- `DAPF_LOCK` clear;
- *(Superseded: all 16 slots are now written every time, unused ones cleared.)* no stale non-empty slot beyond the new set.

Checks before any DART mapping:

- RVBAR base = `0x10000b28000`;
- the DATA-base literal at TEXT+`0x423c` = `0x1f0000ec000`;
- each range is disjoint from System RAM (`region_intersects`), from every
  `/memory` reg and from every `/reserved-memory` child;
- the DVA range is unmapped and inside the aperture;
- everything is page aligned.

The mapping is verified with `iommu_iova_to_phys` at its first and last
pages, and unmapped on unload. Any failed check fails probe **before** the
core is started.

### Sequence (one fresh boot, variant=3, in this order)

The order matters. DAPF state may survive gating, and a `text` run leaves
slot `n-1` set. *(Superseded: every run now writes all 16 slots with a fixed layout — slot 0 TEXT or cleared, slot 1 window, then MMIO — so order no longer matters and a later `control` run clears slot 0 itself.)* The stale-slot check will then refuse a later `control` run
with the same `dapf_mmio`. That refusal is intentional; reboot to rerun the
control.

```sh
sudo dmesg | grep -q "Disabling IRQ #" && echo "REBOOT FIRST"
sudo insmod test/ave-overlay.ko variant=3
cat /sys/kernel/iommu_groups/*/type          # video-encoder group: DMA
sync; sync; sleep 5

# E3a - registers only, core NOT started. Readback must say "verified".
sudo insmod driver/apple-ave.ko stop_after=12 dapf_dump=1 dapf_set=control fw_map_data=1 fw_map_text=2
sudo dmesg | grep -a 'apple-ave\|dapf\|iboot' | tee results/e3a-$(date +%s).log
sudo rmmod apple_ave

# E3b - NEGATIVE CONTROL: no TEXT entry. Core started.
sudo dmesg -C
sudo insmod driver/apple-ave.ko stop_after=15 dapf_dump=1 dapf_set=control fw_map_data=1 fw_map_text=2
sudo dmesg | grep -a 'apple-ave\|apple-dart\|dapf\|iboot' | tee results/e3b-$(date +%s).log
sudo rmmod apple_ave
sudo dmesg | grep -q "Disabling IRQ #" && echo "STOP: reboot before E3c (faults now invisible)"

# E3c - THE RUN: TEXT admitted, nothing at DVA 0xb28000 (discriminates physical vs translated)
sudo dmesg -C
sudo insmod driver/apple-ave.ko stop_after=15 dapf_dump=1 dapf_set=text fw_map_data=1 fw_map_text=2
sudo dmesg | grep -a 'apple-ave\|apple-dart\|dapf\|iboot' | tee results/e3c-$(date +%s).log
sudo rmmod apple_ave

# E3d - only if E3c faulted NO PTE/PMD/TTBR at 0xb28xxx: TEXT also DART-mapped
sudo insmod driver/apple-ave.ko stop_after=15 dapf_dump=1 dapf_set=text fw_map_data=1 fw_map_text=1
```

Use `handshake-test.sh`'s continuous-capture loop or `dmesg -w` rather than
one grep at the end.

### Expected results

E3a, registers only:

- `N entries programmed and verified by readback` and the `E3 after` dump
  shows them: the writes take. Go on.
- `readback MISMATCH`: the layout or the lock semantics are wrong. **Stop**,
  and do not start the core with a filter in an unknown state.

E3b, the negative control, **must** still log
`apple-dart 40d040000.iommu: translation fault: status:0x80000800 stream:0 code:0x800 ... at 0x10000b28200`.

- If it stops faulting, the DAPF writes are not doing what we think.
  **All E3 results are then invalid.** Check the `E3 before`/`E3 after`
  dumps. (Stale entries can no longer admit TEXT: every run rewrites all 16
  slots and clears slot 0 for the control.)

E3c, the run (compare against E3b; the only difference is the TEXT slot):

| observation | reading |
|---|---|
| no fault at `0x10000b28xxx`, and `NO PTE`/`NO PMD`/`NO TTBR` at `0xb28xxx` (or `0x10000b28xxx` with a code other than `0x800`) | The DAPF admits TEXT, and **the DART translates the admitted address by its low 32 bits**. TEXT needs a DART mapping → run E3d. |
| no fault at TEXT at all, then faults at `0x1f0000ecxxx` / DVA `0xecxxx` with `NO PTE` | TEXT fetched **physically** (H1), and the bootstrap reached DATA through the window. The DATA mapping is wrong or absent: check the `iboot DATA` line. |
| no TEXT fault, then `NO_DAPF_MATCH` (code `0x800`) at a **new** address | Progress. The address names the next range the firmware needs. A `0x40d...`/`0x506...` address means MMIO: rerun with `dapf_mmio=adt` or `both` in the same boot (every run rewrites all 16 slots, so switching back needs no reboot). `adt`/`both` admit ave1's probably-gated MMIO: a hang risk. |
| `CPU_STATUS` shows `RUNNING`, scratch or IRQ activity, no faults | H1 confirmed end to end. |
| still `code:0x800 at 0x10000b28200`, readback verified | The entry is present but does not admit. Either r0 `0x11` (ISP's TEXT value) lacks the needed permission here, the inclusivity is wrong, or the DAPF is not the gate. **H1 as programmed is refuted**; next step is a code change (r0 variants), not a rerun. |

E3d is the same table with TEXT mapped: "no fault at `0xb28xxx`" is the
expected success.

### Abort criteria

- Any `REFUSING` line: stop and resolve it.
- E3a readback mismatch; E3b not faulting; `Disabling IRQ #` between runs.
- The desktop stuttering badly for more than a few seconds after `rmmod`, or
  any sign the core is still running after power-off: reboot rather than
  continue.

### Risk

**Moderate.** These are writes to a filter block whose r0 semantics are
inferred.

- A bad write can at worst hang the fabric. Reboot recovers.
- When the core runs, the DAPF admits:
  - iBoot's TEXT copy (outside Linux RAM, recreated by iBoot on the next
    boot);
  - AVE's own MMIO span;
  - the 0x1f0 window. The window's reach is limited by the DART to what is
    mapped: our IPC buffers and, with `fw_map_data=1`, iBoot's DATA, which
    is checked outside RAM and outside every reserved region.
- **If the DART passes admitted addresses through physically** (one of the
  things E3c decides), a stray access in the window would go to
  `0x1f0xxxxxxxx`, where there is no DRAM. That could fault on the bus rather
  than in the DART.
- **`dapf_mmio=adt`/`both` admit ave1's MMIO**, which is probably power-gated.
  A coprocessor access there could hang the fabric. Run `ave0` first.
- It inherits the known post-power-off DART IRQ hazard
  ([31](31-bringup-state.md)).

**Bootability:** unaffected. No DAPF, DART or firmware state persists across
a reboot, and no file outside `results/` is written.

---

## Integration (done, commit after 09d20e9)

Wired into `ave_drv.c`: `ave_dapf_dump()` at stage 8 and again after the
stage-15 liveness sample (both self-gated by `dapf_dump`), and
`ave_dapf_program_selected()` after stage 12, reachable with
`stop_after=12` so E3a really does verify the writes without starting the
core (self-gated by `dapf_set`).

Guards added after review (2026-09-13):

- `dapf_set=` other than `off` is refused at probe unless `fw_map_text` is 1
  or 2, so an admitted TEXT fetch can never run our own (different) image.
- The AVE IRQ is requested disabled and enabled only once VENC is powered;
  every power-off (stage 15, remove, probe failure, devres) disables it
  synchronously first, because gating is asynchronous and the handler reads
  SVE registers.
- Any probe failure after stage 9 unwinds FwIPC, the firmware buffer and the
  iBoot DART mappings, so a refused E3 run (e.g. the stale-slot refusal) no
  longer leaves DATA mapped and blocks a rerun in the same boot.

## Results

### E1 — run 2026-09-13, `results/e1-1789299549.log`

Camera streaming, ISP runtime-active, all 10 ISP domains on; `isp_peek`
pinned them and read only.

- **ISP's RVBAR is `0x0102010000c68001`: locked, base = ISP TEXT physical**
  (`0x10000c68000`, its `/reserved-memory` carve-out mapped at IOVA 0).
  CPU_STATUS `0x2d` RUNNING|IDLE, CPU_CONTROL `0x10`. So a locked RVBAR
  pointing at physical TEXT is how a *working* ASC on this machine boots.
  AVE's `0x0102010000b28001` is the same pattern, not an anomaly.
  **Confirmed.**
- **ISP's DAPF slot 0 admits exactly its TEXT, physically:**
  `0x10000c68000 - 0x100015e7ffc`, **r0 `0x11`**, r4 1. Slot 1 is the
  `0x1f0` window (r0 `0x33`), then MMIO windows (r0 `0x31`), 16 slots in
  use. The TEXT entry is not in the restore ADT (0/15 slot-for-slot matches,
  everything shifted by one); it is injected, presumably from the live ADT.
  **Confirmed.** This is the entry AVE lacks.
- **End addresses are stored with the low two bits clear:** ADT
  `0x1f0ffffffff` reads `0x1f0fffffffc`, `0x28e584043` reads `0x28e584040`.
  Inclusive, last admitted 4-byte word. **Confirmed.**
- **The t8020 register layout is right**: every entry decodes to a sensible
  range, validating the offsets `ave_dapf.c` uses. **Confirmed.**
- ISP's DART: TCR[0] `0x80` TRANSLATE, all other SIDs 0, DAPF_LOCK 0.
- **Inferred:** ISP's DAPF was programmed by m1n1 at boot and ISP had been
  runtime-suspended (domains off) until the camera started, so **DAPF
  contents survive power gating.** And ISP's TEXT is DART-mapped at IOVA 0,
  not at the low 32 bits of `0x10000c68000` (`0xc68000` falls inside its
  DATA mapping), so an admitted physical fetch is most likely passed through
  untranslated — H1, favouring `fw_map_text=2`. E3c is still the test.

### E2 — run 2026-09-13, `results/e2-v2-*.log`

Overlay `variant=2` (no DART bound), `stop_after=8 dapf_dump=1`.

- **AVE's DAPF holds uninitialised contents**: all 16 slots non-empty with
  start above end, arbitrary r0/r4 (e.g. slot 0 `r0 0x200 r4 0xe3
  0x010000d0000 - 0x0000e040400`). The DART's TTBRs are likewise
  non-physical values; TCR[0/1/15] read `0x80`; PARAMS1 `0x1ed01020`,
  PARAMS2 `0x00021037`; REMAP the identity pattern.
- **Identical after a full VENC power cycle** (rmmod, `venc_sys off-0`,
  insmod; `results/e2-v2-repeat-*.log`), apart from the ERROR address. With
  E1 this means the state is retained across gating and nobody has written
  it since cold reset. No TEXT admission from iBoot survives. **Confirmed.**
- Consequently E3 was changed (commit after 01d36f1): write **all 16 slots**
  in a fixed ISP-like layout (slot 0 TEXT or cleared, slot 1 window, then
  MMIO, rest cleared; a cleared slot gets r0 written first), store ends
  already masked, use **r0 `0x11`** for TEXT, and drop the stale-slot
  refusal and `dapf_allow_stale`.
- E2c (`variant=3` control) is folded into E3a, whose stage-8 dump runs on
  `variant=3` before anything is programmed.
- **Range alone does not admit.** E2's slot 5 (`r0 0x100 r4 0xac2
  0x04004a24100 - 0x10014604100`, start below end) covers TEXT+0x200 by
  range, yet every earlier run faulted NO_DAPF_MATCH there. Admission
  therefore depends on the r0/r4 bits, which is why E3 copies ISP's TEXT
  values rather than any range-only reasoning. *Inferred*: it assumes the
  garbage was the same on those earlier boots, as it was across E2's power
  cycle.
- Each slot is now disabled (r0 = 0) before being rewritten, enabled slots
  included (review of 8958777).
