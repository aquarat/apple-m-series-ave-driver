# 79. Porting to another Apple Silicon machine

2026-09-24. How to take the driver from the t6001 (M1 Max, MacBookPro18,4
`j314c`) it was brought up on to another SoC, with the M1 Mac mini
(Macmini9,1, `j274`, t8103) as the worked target. Short version: the
command protocol, the H.264/HEVC session code and the V4L2 layer are
SoC-independent. The per-SoC facts are in one table, `driver/ave_soc.c`.
The device-tree overlay and a few tools are still t6001-only and need a
sibling per SoC.

## 1. What is shared, and why

| layer | shared across SoCs? | why |
|---|---|---|
| Command ABI (`ave_abi.h`, `ave_cmd.c`) | **yes**, per macOS version | the wire layouts are the `AppleAVE2` kext's, one binary for every M1-family Mac in 13.5 |
| Session, rate control, V4L2 (`ave_session.c`, `ave_v4l2.c`) | **yes** | pure protocol and buffer logic |
| Boot handshake (`ave_ipc.c`, `ave_abi_boot.h`) | **yes**, except the device row | the row (DevID/DevType/ChipType) is per SoC, in the table |
| Register banks | **from the DT node** | `reg` entries of the overlay's node, by index/name |
| DART/DAPF/SMMU, iBoot placement, power nodes, PMP | **no**: `ave_soc.c` | addresses and paths, below |
| Firmware image | **maybe** | `apple/ave_h13c.bin` is the 13.5 image for this Mac. Another Mac may ship a different variant (§3 step 2) |
| Device-tree overlay (`test/ave-overlay-e4.dts`) | **no** | t6001 addresses, AIC IRQs 1028/1031, DART stream IDs |
| m1n1 DAPF patch (`tools/m1n1/`) | **probably** | ADT-driven (`/arm-io/dart-ave0`, reg index 3, `/arm-io/ave0`); node names must be checked |
| PMP vote (docs/75, docs/78) | **no** | needs Asahi's PMP on that SoC, see §4 |

## 2. The table (`driver/ave_soc.c`)

The node's first compatible selects the row (`ave_of_match` in
`ave_drv.c`; the overlay says `"apple,t6001-ave", "apple,ave"`). A node
that matches only `"apple,ave"` is refused at probe with a pointer here.

| field | t6001 value | where it comes from | notes |
|---|---|---|---|
| `name` | `"t6001"` | | logs |
| `fw_name` | `apple/ave_h13c.bin` | IPSW `AppleAVE2FW_*.im4p`, unwrapped (`tools/fetch_firmware.py`) | §3 step 2 |
| `fw_pristine_name` | `apple/ave-13.5-data-pristine.bin` | `tools/make_ave_data_blob.py` | only for `fw_restore_data` |
| `dev[13.5]` | 14 / 11 / 8 | kext table `0xfffffe0007bc2a38` (docs/09) | **t6001's own row is 15/12/9**; the driver has always sent t6000's and it works. **t8103: 13/10/7** [C, docs/09] |
| `dev[26.6]` | 11 / 9 / 6 | docs/45 row 3 | 26.6.2 ABI is not the operating one |
| `cpudart_phys`, `dart1_phys`, `smmu_phys`, `dapf_phys` | `0x40d040000`, `…30000`, `…20000`, `…44000` | ADT `dart-ave0` reg[0..3] + `/arm-io` ranges (`tools/check_addrs.py`) | DAPF = CPUDART + 0x4000 is checked |
| `dapf_window`, `dapf_mmio_own`, `dapf_mmio_adt` | docs/44 addendum | ADT `dart-ave*` `filter-data-instance-*`, masked | only the driver-side DAPF experiments use these; m1n1 programs the real DAPF |
| `iboot.*` | TEXT `0x10000b28000`/`0xec000`, DATA `0x10001a90000`/`0x134000` | the live RVBAR and docs/43-44 | **per machine and boot chain**, not just per SoC; the driver checks the live RVBAR against `text_phys` first |
| `me1_node` | `/soc/power-management@28e580000/power-controller@8020` | Asahi DT, label `venc_me1` | checked by label before use |
| `pmp_report_node` | `/soc/pmp_report@28e3c0000/report@10` | Asahi DT, label `pmp-venc-sys` | checked by label |
| `pmp_ps_reg`, `pmp_report_base`, `pmp_dvfs_wr/rd`, `pmgr_perf_blk` | `0x28e0802d8`, `0x28e3c0000`, `0x28e3d0888`/`0x28e3c1110`, `0x28e5d8000` | `tools/pmp_ptd_map.py` from the ADT (docs/75) | 0 = unknown: `perf_dump`, `pmp_report` and `pmp_vote` then refuse instead of guessing |

## 3. Bring-up checklist for a new machine (M1 Mac mini as the example)

Static first, on the host, with the new Mac's IPSW:

1. **ADT.** `tools/fetch_firmware.py --board j274 --list`, then extract
   `DeviceTree.j274ap.im4p` and dump it with `tools/adt_dump.py --grep
   'ave|dart-ave|pmp'`. Record `/arm-io/ave*`, `/arm-io/dart-ave*` (reg,
   instance, sids, `filter-data-instance-*`), the pmgr `ps-regs`, and
   `/arm-io/pmp`. `tools/check_addrs.py` converts bus addresses to CPU
   physical ones. The `/arm-io` ranges offset is not the same on every SoC.
2. **Firmware.** `fetch_firmware.py --list` shows the `AppleAVE2FW_*`
   member for that board. If it is `H13C` and hashes the same as
   `data/blobs/AppleAVE2FW_H13C.im4p`, nothing changes. If it is a
   different variant, the firmware-side facts need re-checking:
   - the assert line numbers the driver logs
   - MCPU image layouts (the session diagnostics)
   - the firmware VAs cited in docs/77
   - `iboot.*` sizes
   The command layouts come from the kext and stay the same.
3. **Device row.** Take the SoC's row from the kext table (docs/09). t8103 on
   13.5 is DevID 13, DevType 10, ChipType 7. Note that t6001 works with
   t6000's row; if the new SoC's own row fails at Config, try its sibling's.
4. **Table row.** Add `ave_soc_t8103` to `ave_soc.c` and its compatible
   (`"apple,t8103-ave"`) to `ave_of_match`. Leave the PMP fields at 0 until
   §4 is settled.
5. **Overlay.** Copy `test/ave-overlay-e4.dts` to a t8103 sibling:
   - compatible `"apple,t8103-ave", "apple,ave"`
   - the node's `reg` (DPE, ASC, SVE, PMGR PS, fabric, cpudart, dapf, smmu) from step 1
   - the AIC IRQs
   - the DART nodes and their shared IRQ
   - the stream IDs from the ADT `sids`

   `test/ave_overlay_mod.c` resolves the power domains by **label** at load
   time (venc_sys, venc_pipe5, venc_me0, venc_pipe4, afnc4_ioa). Check the
   t8103 DT has those labels (Asahi `t8103-pmgr.dtsi`). afnc4_ioa is a
   t6001 fabric domain that happened to be listed; its t8103 counterpart,
   if any, is a question for the ADT's `ave0` `power-gates`. The module's
   AIC (0x13) and venc_sys (0x1d) phandle checks are stock-t6001 numbers
   and must become label lookups, or the new DT's numbers.
6. **m1n1.** The DAPF patch walks the ADT (`/arm-io/dart-ave0`, reg index 3,
   TEXT entry from `/arm-io/ave0`). Check those names and indices exist on
   t8103. Build it, keep `boot.bin.pre-ave` and a restore script on the
   ESP (docs/50), and boot it once before loading anything.
7. **Lab.** Put the new machine's addresses in `lab.env`/`lab.local.md`
   (git-ignored). The Mac mini has built-in Ethernet, so netconsole needs no
   USB adapter (the cdc_ncm batching workaround in `e3-run.sh` is then
   moot).

Then on hardware, one step per boot, as docs/53 did:
1. probe up to the firmware boot (`stop_after` low)
2. Config/Open
3. the self-test frame (`session_selftest=1 session_frame=1`)
4. V4L2 (`tools/v4l2-test.sh`)
5. compliance

## 4. Performance (the PMP)

On t6001 the encoder's clock follows a vote to the PMP coprocessor
(docs/75, docs/78, f95-f99: 2.4-2.6x). That needs:
- Asahi's PMP driver running on that SoC
- a `pmp-report` entry for VENC
- the vote's dashboard address

Asahi's `apple-pmp-report` currently matches only `t6000-pmp-v2-report`,
`t6020-…` and `t8112-…`. **There is no t8103 entry**, so on an M1 Mac mini
the encoder runs at whatever clock the boot leaves. Whether the M1 has the
same mechanism (PMP v1?) is open. Until it is traced, leave the table's PMP
fields 0. The driver then ignores `pmp_report`/`pmp_vote` with a warning,
and never writes to t6001 addresses.

**A kernel without the PMP is fine.** With the defaults (`pmp_report=0`,
`pmp_vote=0`) the driver never touches the PMP, as in every run up to f88
on stock Fedora. With the PMP options set but no running PMP, or with
`report@10` disabled, or a SoC row without PMP fields, probe warns and
carries on at the boot clock. `pmp_vote` is never written without the
report held. `perf_dump` only reads, and only after the PMP's power state
reads on. Any new MMIO write must use a non-posted
mapping (`ioremap_np`), because `/soc` is `nonposted-mmio`, and a posted
write is an SError (f93/f94).

## 5. Still t6001-specific outside the table

- `test/ave-overlay-*.dts` and the phandle checks in `test/ave_overlay_mod.c`
  (§3 step 5), and its `pmp_venc` path `/soc/pmp@28e700000`.
- Log strings in `driver/ave_session.c` that print t6001 addresses (e.g.
  `0x40D130240`) next to bank-relative reads. They are cosmetic, and the
  reads themselves use the DT banks.
- `tools/pmp_ptd_map.py` control C1 compares against t6001's VENC_SYS PS
  address.
- `AVE_ARM_IO_BUS_OFFSET` in `ave_hw.h` (unused; t6001's `/arm-io` offset).
- The overlay's variants 0-3 and 5 exist for history and are not needed on
  a new machine.
