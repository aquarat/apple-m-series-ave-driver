# 86. System suspend and resume (s2idle)

2026-09-27. Design, failure modes and a hardware test plan for `pm_sleep`,
the driver's system-sleep support. **Nothing here has run on hardware.** It
is written from the code and from what docs/63, docs/82 and docs/84 have
established. Tags follow docs/00: **[C]** confirmed on hardware or from a
binary, **[I]** inferred, **[U]** unknown. AGENTS.md applies: only the lead
runs §6.

---

## 0. Summary

| question | answer |
|---|---|
| Default | **`pm_sleep=0`**: nothing changes. No notifier is registered and the dev_pm_ops callback returns 0, exactly as the driver behaved with no PM ops at all. |
| Why off by default | Untested. The lab machine never sleeps (AGENTS.md, "The target never sleeps"), and whether s2idle even keeps the machine alive with the AVE overlay applied is itself untested (§6 run a2). |
| `pm_sleep=1` | Refuse any system suspend while an encoder is powered. It touches no register. **This is the safe setting for any machine that can sleep**, until `pm_sleep=2` passes §6. |
| `pm_sleep=2` | Before the freeze: end the open stream, halt the core (or reset a wedged one), unmap everything while still powered, then drop power. After resume: bring the firmware back through probe's stages 6-16, the path a reload already takes (docs/84 §4, R5/R6). |
| A stream running at suspend | **Fails with EIO.** It is not resumed (§3). The client must stop both queues and start again. Open file handles and the V4L2 node survive. |
| A hung firmware at suspend | Bounded: no command is sent to it, the block reset is pulsed (R7/R8's recovery), and the sleep proceeds if the core then reads STOPPED. Otherwise the suspend is aborted and the device recovers on its last close (docs/84 §5). `pm_hung_reset=0` aborts instead of resetting. |
| Does the DAPF survive s2idle? | **[U].** Resume compares the DAPF with a fingerprint captured at probe. If it changed, it writes back the probe-time contents (ave0: the 16 slots as m1n1 left them; ave1: `ave_dapf_program_instance`), non-posted, and requires the fingerprint to match before any core starts. |
| Both encoders | Each instance has its own notifier and teardown. Resume boots run one at a time (a global mutex), like probe. |

## 1. Why not dev_pm_ops alone, and why not re-probe

**Not a re-probe.** `ave_schedule_recover()` (`device_reprobe`) already takes
the device through remove and probe, and probe's stage 7 already knows how to
reset a halted core and restore its DATA (R5-R8). But remove unregisters the
V4L2 node, and doing that under an open file handle is a use-after-free,
which is why docs/84 §5 waits for the last close. A suspend cannot wait for
userspace to close anything. So resume reuses probe's *stages* (factored out
as `ave_boot_core(ave, resume)`), not probe itself: stages 1-5 (mappings, IRQ,
power-domain attach, runtime PM) and the V4L2 registration stay in place.

**Not only dev_pm_ops.** The teardown and the bring-up both need a running
system:
- the firmware conversation: seconds of IPC, with the IRQ live;
- `request_firmware()` for the image and the pristine DATA blob;
- `device_add()` of the venc_me1 and PMP holder devices;
- runtime PM of those holders.

Inside `.suspend`/`.resume` the tasks are frozen and the device's children
are already suspended. So:

| hook | when | does |
|---|---|---|
| PM notifier, `PM_SUSPEND_PREPARE` (and `HIBERNATION_PREPARE`, `RESTORE_PREPARE`) | before tasks freeze | the whole teardown (`ave_pm_prepare`). An error aborts the suspend before anything is frozen. |
| PM notifier, `PM_POST_SUSPEND` (and the other two POSTs) | after tasks thaw | queues `ave_pm_work`, the resume boot, on `system_unbound_wq` |
| `dev_pm_ops.suspend` (`ave_pm_suspend`) | device suspend | refuses (-EBUSY) if the encoder is still powered and `pm_sleep != 0`. This is the last line of defence against genpd gating the domains under a live core in the noirq phase. |

By the time genpd's noirq phase gates the venc domains, the core is stopped,
nothing is mapped, the IRQ is disabled, and the driver holds no power
reference. So whatever s2idle does to those domains, it does it to an idle
block.

## 2. The sequence

### 2.1 Suspend (`ave_pm_prepare`, `ave_pm_quiesce_core`)

1. Wait for a resume boot still in progress (`flush_work`).
2. Return 0 if the encoder is not powered or not in state ON. That covers
   stop_after=15 loads (which power off in probe), a device already off, and
   a DEAD device (§4).
3. `pm_sleep=1`: refuse (-EBUSY).
4. Refuse if `stop_after < 16` (a staged load cannot be brought back), or if
   probe could not capture the DAPF.
5. Set the state to SUSPENDING. From here on, `ave_enc_start` returns EBUSY
   and `ave_enc_encode` returns EIO.
6. V4L2 (`ave_v4l2_pm_quiesce`):
   - `v4l2_m2m_suspend`: no new jobs, and wait for the running one. That job
     is bounded by the Process timeout (2 s, which also marks the firmware
     hung).
   - Take `hw_mutex`, and keep it through step 9.
   - If a context owns a session: `ave_enc_stop` (Stop then Close, each
     bounded at 2 s, skipped with EIO on a hung firmware). Then mark the
     context `pm_lost` and put both of its vb2 queues in error.
7. If a client is still open (the self-test's, or a V4L2 Close that
   failed) and the firmware is not hung: `ave_session_close_client`.
8. If Config ran, no client is open and the firmware is not hung: Halt
   (`ave_session_halt`, scratch 0 polled for 1 s).
9. Read CPU_STATUS. If STOPPED is not set:
   - `pm_hung_reset=1` (default): `ave_core_pulse`, the stage-7 pulse with
     the DAPF read before and after. The core must then read STOPPED.
   - Otherwise, fail.
10. `ave_release_all`:
    - disable and synchronise the IRQ, quiesce the SMMU watch;
    - free the session buffers;
    - `ave_fw_unload`: unmap iBoot DATA (or free ave1's owned DATA), free the
      image;
    - `ave_ipc_fini`.
    All of this runs while powered: docs/63's "unmap while powered, gate
    last".
11. Forget the firmware state:
    - clear `running`, `mcpu_created`, `client_open`, `fw_hung`,
      `recover_halted`, `asc_started`, `hs_seen`, `dapf_programmed`;
    - `ave_power_off(ave, "system suspend")`: ack the SVE status, release
      the vote, the PMP report and venc_me1, then `pm_runtime_put_sync`.
    - State OFF.
12. Drop `hw_mutex`. The m2m queue stays paused until resume.

On failure at 9 (no reset line, DAPF unreadable, or the core still not
STOPPED after the pulse):
- the state returns to ON, and `fw_hung` is set if anything reached the
  firmware or the block;
- m2m is resumed and the suspend is aborted with -EBUSY;
- if no file handle is open, the re-probe is scheduled at once; otherwise it
  runs on the last close (docs/84 §5).

The stream is lost either way.

**Every wait on this path is bounded.** Worst case with a firmware that has
wedged without being marked hung:
- 2 s for the running job's Process;
- 2 + 2 s for Stop and Close in `ave_enc_stop`;
- 2 + 2 s again from step 7 (the client is still open);
- the pulse's 200 ms settle.

That is about 10 s, before the freezer starts, so no freezer timeout is
involved. A hung firmware already marked `fw_hung` goes straight to the
pulse.

### 2.2 Resume (`ave_pm_post`, `ave_pm_work`)

`PM_POST_SUSPEND` queues the work only if this driver took the device to
OFF. The work runs with the global boot mutex and `hw_mutex` held. A STREAMON
arriving meanwhile waits on `hw_mutex` and then succeeds, rather than
failing.

1. Forget the firmware state again, in case anything was left.
2. `ave_power_up(ave, false)`: stage 6 without re-registering the devres
   action. It takes the runtime-PM reference, enables the IRQ, adds a new
   venc_me1 holder, and redoes the PMP report and the vote setup.
3. `ave_boot_core(ave, true)`, which is stages 7-16:
   - **stage 7**: re-arm the SMMU watch (its IRQ is devres and still
     requested). Write SVE+0x38. Then **`ave_dapf_pm_restore`**: compare the
     DAPF fingerprint with probe's. If it is unchanged, nothing is written.
     If it changed, write it back and require the probe-time fingerprint on
     readback. Then `ave_core_reset(..., resume)`:
     - ave0: `ave_fw_data_ran` finds the drifted DATA, even with
       `reload=0`, pulses the reset and sets `recover_halted`.
     - ave1: CPU_STATUS 0x2e (halted) means pulse; 0x2a (lost power, cold)
       means no pulse, because its DATA is rebuilt anyway.
     - Then the datapath DART is re-mirrored from the CPUDART, even without
       a pulse. Nothing is written when it already matches.
     - Then the DPE tunables.
   - **stage 8**: the ASC read, and ave1's DAPF programming as in probe.
     There is no new capture: the probe-time one stays the reference.
   - **stages 9-13**: FwIPC; `ave_fw_load`, whose placement check refuses if
     RVBAR no longer points at iBoot's TEXT; the boot scratch writes; the
     DATA restore (forced by `recover_halted`); the ASC start.
     `asc_started` is set just before the RUN write.
   - **stages 14-15**: message 1 and the 200 ms activity check. The
     2000-sample liveness histogram is skipped.
   - **stage 16**: the handshake. Then Config (`ave_enc_init`) whenever probe
     sent one, so the next Halt has its controller (fw 0x10d44). V4L2 is not
     registered again and the self-test is not run again.
4. On success: state ON, then "encoder ready again after N ms".
5. Drop the locks and call `v4l2_m2m_resume`.

On failure, see §4 (DEAD, or ON with `fw_hung`).

## 3. The stream at suspend: failed, not resumed

A session is firmware state: the client, its DPB, its rate-control history,
its reference frames. All of it goes with the Halt, and the reset after it.
Resuming "cleanly" would mean opening a new session with the same
configuration and forcing an IDR. The output would then be a new stream
spliced onto the old one: new SPS/PPS, a POC restart, and rate control from
cold. With B-frames or two references, the frames in flight would also have
to be replayed. A client that did not ask for that would get it silently. So:
- `ave_v4l2_pm_quiesce` stops the session before the sleep and puts both of
  the owner's vb2 queues in error, so DQBUF and poll give EIO/EPOLLERR.
- Any job still queued runs after resume, finds no session, and completes
  with ERROR. Because `pm_lost` is set, it re-asserts the queue error.
- STREAMOFF clears the error (vb2). A later STREAMON of both queues starts a
  fresh session. `pm_lost` is cleared when it does.

ffmpeg's `h264_v4l2m2m` treats this as a fatal encode error, as it would a
firmware hang. A transparent restart is possible future work, not a
correctness issue.

## 4. Failure modes

| # | situation | detection | behaviour | a reboot needed? |
|---|---|---|---|---|
| F1 | STREAMON between PREPARE and the freeze | state != ON under `hw_mutex` | EBUSY | no |
| F2 | job running at suspend | m2m | waited out (≤ 2 s); a timeout marks the firmware hung → F4 | no |
| F3 | Stop/Close time out | `ave_enc_stop` / `close_client` return | no Halt; pulse (F4) | no |
| F4 | firmware hung, or Halt did not land | `fw_hung`, or CPU_STATUS without STOPPED | `ave_core_pulse`, then CPU_STATUS must read STOPPED | no, if the pulse works (R7/R8) |
| F5 | pulse impossible (no reset in the DT, DAPF unreadable) or still not STOPPED; or `pm_hung_reset=0` | pulse return, CPU_STATUS | suspend **aborted** (-EBUSY); `fw_hung`; re-probe on the last close | no |
| F6 | another device or notifier aborts the suspend after ours | POST_SUSPEND arrives | normal resume boot | no |
| F7 | DAPF changed across the sleep | fingerprint vs probe's | written back from probe's capture, verified | no, if the write-back reads back |
| F8 | DAPF write-back does not read back, or DAPF_LOCK is set | readback / lock bit | resume fails before the core starts → DEAD (power off). The re-probe at the last close will also refuse at stage 7 ("the DAPF changed"). | **yes** |
| F9 | RVBAR lost iBoot's value (its lock reset with power) | `ave_fw_check_iboot_placement` in `ave_fw_load` | DEAD. RVBAR survived every earlier power-down of venc_sys (`ave_drv.c` rvbar_probe notes) **[I]** | **yes** |
| F10 | handshake or Config fails after the core started | stage 14/16 error with `asc_started` | stays powered (docs/63), `fw_hung`; the re-probe on the last close takes R7's path (leak, keep power, the new probe pulses) | no [I] |
| F11 | `request_firmware` or allocation fails at resume | stage 10 error | DEAD; the re-probe tries again | maybe |
| F12 | rmmod or re-probe during suspend/resume | `unregister_pm_notifier` waits out a running callback; `cancel_work_sync` on the boot | remove on an OFF/BOOTING/DEAD device only unregisters V4L2 and returns | no |
| F13 | the machine dies in s2idle itself: domains cycling, the NIC | not detectable by the driver | §6 a1/a2 separate the machine from the driver | — |

DEAD means the power is off and nothing is mapped. `fw_hung` is set, so
starts fail with EIO. `ave_schedule_recover` re-probes the device when no
handle is open: at once, or on the last close. `remove()` treats DEAD like
OFF.

## 5. Known risks and open questions

1. **What s2idle does to venc_sys [U].** genpd gates a domain in the noirq
   phase when all its devices are suspended, whatever their runtime state.
   The DARTs are apple-dart devices in venc_sys, and they save and restore
   their TCR/TTBRs themselves. The DAPF survived Linux's boot-time gating
   (docs/50), so "the DAPF survives" is the expected outcome [I]. The
   resume check exists because it is not known.
2. **The venc_sys ON→OFF cycle** was once the leading hypothesis for resets
   (docs/58 §5). f38 later found a different killer, but the hypothesis was
   never tested on its own. s2idle cycles these domains even with no driver
   loaded, which is what §6 a2 tests.
3. **Freeing buffers after a pulse of a hung core.** R7 leaked them instead.
   Here the pulse resets the block, which should stop every master, before
   anything is unmapped [I]. §6 d1 is the test.
4. **Pulsing a core that lost power** (cold, reads 0x2a) happens for ave0
   whenever its DATA drifted. s2-9 was once blamed on exactly that, then
   re-attributed to the overlay window (docs/53) [I].
5. **Two resume boots** run one after the other, never concurrently [C,
   code]. Their ordering follows the notifier order, which is probe order.
6. **Hibernation** takes the same notifier path. Asahi does not support
   hibernation, so it is untested and likely unreachable.
7. **Contiguous memory** for the firmware image
   (`DMA_ATTR_FORCE_CONTIGUOUS`) is allocated again on every resume, as on
   every reload. A failure there is F11.
8. **The netconsole NIC did not survive s2idle** on this machine (AGENTS.md).
   The first runs may leave only the local journal.

## 6. Hardware test plan (for the lead; docs/53 style)

Standing rules:
- Fresh boot per run. One variable per run. Repeat a "yes" before believing
  it. Record every run in docs/53.
- `rtcwake -m freeze` writes `/sys/power/state` directly, so logind's masks
  do not stop it. **The operator must be present for a1 and a2:** whether the
  RTC alarm can wake this machine is unknown. If 30 s pass with no wake,
  press a key or the power button (briefly) and note which one worked.
- The netconsole NIC may not come back on resume. Before every run, start a
  local capture that survives a normal resume: `dmesg -w > /var/tmp/NAME.kmsg
  &` plus `journalctl -k -b` afterwards. Collect it over Wi-Fi if the wired
  address is dead.
- A reset during s2idle loses the local tail. The receiver then has
  everything up to "PM: suspend entry".

Grep, in every run:
```
grep -E 'PM: suspend (entry|exit)|Freezing|Some devices failed|pm: |powered off|halt: |core reset: |reload: |handshake complete|encoder ready|v4l2: ' NAME.kmsg
```

### a1: s2idle, no overlay, no driver

**For:** does this machine come back from s2idle at all, and what wakes it?
Everything later depends on it.

How: fresh boot, nothing AVE-related loaded. `sudo rtcwake -m freeze -s 30`.

- **yes:**
  - the machine resumes;
  - "PM: suspend entry (s2idle)" is followed by "PM: suspend exit";
  - SSH answers (wired or Wi-Fi);
  - note whether the NIC came back and what woke the machine.
- **no:**
  - no wake after 30 s and after a keypress, or a reset;
  - the problem is the platform. Stop here: nothing in the driver can be
    tested until this passes.

Repeat once.

### a2: s2idle with the overlay applied, no driver

**For:** the venc_sys ON→OFF cycle with the DARTs bound (docs/58 §5), without
our driver.

How: overlay only (`insmod test/ave-overlay.ko variant=7`; wait until the
DARTs have bound), then `rtcwake -m freeze -s 30`. Afterwards, read
`pm_genpd_summary | grep venc`.

- **yes:**
  - it resumes as in a1;
  - apple-dart logs nothing new;
  - no "nobody cared" for the DART IRQ;
  - the venc domains read as they did before the sleep.
- **no:**
  - a reset or a hang here means s2idle with the AVE DARTs bound is
    unsafe on its own;
  - the driver cannot fix that. The only safe setting is then `pm_sleep=1`,
    which refuses the suspend while the encoder is up; the overlay alone
    would still be exposed.

### b0: control, `pm_sleep=2`, no sleep

**For:** the new code changes nothing until a suspend.

How:
```
tools/ave-load.sh pm_sleep=2
tools/v4l2-test.sh 60 ctl        # on each node
```

- **yes:**
  - the probe log is identical to the last baseline, except "pm: DAPF
    captured for resume: fingerprint ..., TEXT fetch admitted" and
    "pm: pm_sleep=2 (...)" per instance;
  - H.264 at 44.308053 dB; HEVC byte-identical to R6.
- **no:**
  - "pm: DAPF not readable": the overlay lacks the "cpudart"/"dapf" regs,
    and suspend would be refused;
  - a PSNR change: the refactor changed probe. Diff the logs before any
    sleep.

### b1: driver loaded and idle, `pm_sleep=1` (refusal control)

**For:** the refusal path, with zero register access.

How: `tools/ave-load.sh pm_sleep=1`, then `rtcwake -m freeze -s 30`.

- **yes:**
  - `rtcwake` fails at once;
  - "PM: suspend entry (s2idle)", then "pm: refusing the system suspend:
    the encoder is powered and pm_sleep=1", then "PM: suspend exit", with
    no "Freezing user space" in between (the notifier runs before the
    freeze);
  - the machine stays awake and the next `v4l2-test.sh 60 ctl` is at
    baseline.
- **no:**
  - the machine sleeps anyway: the notifier was not registered. Look for
    "pm: pm_sleep=1".

### b2: driver loaded and idle, `pm_sleep=2`, both encoders

**For:** the full path with no stream.

How: `tools/ave-load.sh pm_sleep=2`, then `rtcwake -m freeze -s 30`, then
`v4l2-test.sh 60 ctl` on `apple-ave-enc` and on `NODE=apple-ave1-enc`.

- **yes, before the sleep, per instance:**
  - "pm: system suspend: stopping the encoder";
  - "halt: scratch 0 = 0x08042006, the firmware reached its wfi";
  - "pm: core stopped (CPU_STATUS 0x0000002e)";
  - "powered off (system suspend)" and "pm: encoder off for the sleep";
  - no "core reset" lines.
- **yes, after the sleep, per instance, one after the other:**
  - "pm: resume: bringing the firmware back";
  - "pm: resume: DAPF fingerprint ... SURVIVED" **or** "CHANGED ...;
    writing it back" followed by "restored from Linux and read back". Record
    which one: it answers §5.1;
  - ave0: "reload: DATA differs from pristine in ~290000 byte(s)", then a
    core reset whose DAPF reads SURVIVED and admitted, then "restored
    0x134000 bytes";
  - ave1: "reload: CPU_STATUS 0x2e" (or 0x2a if it lost power);
  - "handshake complete", then "pm: resume: encoder ready again after N ms";
  - then both tests at baseline: 44.308053 dB, HEVC byte-identical.
- **no:**
  - "pm: resume failed (...) before the core was released": DEAD. The log
    says which stage. For F8 and F9 note it and reboot.
  - "after the core was released": close all handles; the re-probe must
    then recover (R7 path).
  - A reset during resume: the netconsole or journal tail names the stage.

Repeat b2 in the same boot at least three times. A second sleep must work
like the first.

### c: during a stream, both encoders

**For:** the stream-failure policy, and that a stream in flight is stopped
cleanly.

How:
- start a long stream on each node in the background:
  ```
  W=1920 H=1080 tools/v4l2-test.sh 3000 ctl &
  NODE=apple-ave1-enc tools/v4l2-test.sh 3000 ctl &
  ```
- after about 3 s, `rtcwake -m freeze -s 30`;
- after resume, wait for both clients to exit, then run `v4l2-test.sh 60 ctl`
  on each node.

- **yes, before the sleep, per instance:**
  - "pm: system suspend during a stream: session ended (Stop/Close 0)";
  - then b2's halt lines, with no "core reset".
- **yes, after the sleep:**
  - b2's resume lines;
  - both background clients end with an error (v4l2-ctl reports EIO on
    DQBUF or poll) and not with a hang;
  - "v4l2: frame N failed: -5" is allowed for jobs still queued;
  - the two new 60-frame tests are at baseline.
- **no:**
  - "Stop/Close -110" (timeout): the firmware did not return the client
    mid-stream. Expect the pulse lines. Record, and repeat c.
  - A client that hangs after resume instead of failing: the queue error did
    not reach it. That is a driver bug.
  - A new test below baseline: DATA or the DART mirror are wrong after
    resume. Compare with b2.

### d: suspend with a hung firmware

**For:** the bounded path and the pulse.

- **d1** (`pm_hung_reset=1`, the default):
  - force the hang as R8 did (docs/84 §5, docs/53 "R8": RefSpacingP 2
    through `/sys/module/apple_ave/parameters/session_ref_spacing_p` on an
    H.264 stream);
  - keep the hung client's handle open (let v4l2-ctl sit, or hold it with
    `sleep` on a separate fd);
  - `rtcwake -m freeze -s 30`.
- **d2**: the same, with `pm_hung_reset=0`.

- **yes (d1), before the sleep:**
  - "enc: frame N timed out ... hung", then "pm: system suspend: stopping
    the encoder";
  - no Stop, Close or Halt lines;
  - "pm: CPU_STATUS 0x...: the core did not stop; pulsing the block
    reset";
  - "core reset: CPU_STATUS now ... STOPPED", then "pm: core stopped";
  - all of it within about 3 s of "PM: suspend entry";
  - the machine sleeps.
- **yes (d1), after the sleep:**
  - b2's resume lines;
  - the hung client gets EIO;
  - after it closes, and with `session_ref_spacing_p` back at 0, a new
    60-frame test is at baseline.
- **yes (d2):**
  - "pm: CPU_STATUS ...: the core did not stop, and pm_hung_reset=0";
  - "aborting the system suspend";
  - `rtcwake` fails and the machine stays awake;
  - after the hung client closes: "recover: re-probing after a firmware
    hang", then R8's recovery.
- **no:**
  - the suspend takes much longer than about 10 s: an unbounded wait.
    Capture `echo w > /proc/sysrq-trigger` if it is still alive.
  - "pm: CPU_STATUS ... after the reset: still not stopped": the pulse did
    not stop the core; F5 behaviour is expected.
  - A reset right after "pm: core stopped": freeing after a pulse is not
    safe (§5.3). Make d1 leak instead, the way R7 does.

## 7. Code map

- `driver/ave_drv.c`:
  - `pm_sleep` and `pm_hung_reset`;
  - `ave_power_up` (stage 6);
  - `ave_boot_core` (stages 7-16, `resume`);
  - `ave_core_pulse` (split out of `ave_core_reset`);
  - `ave_release_all`, `ave_pm_forget_core`, `ave_pm_quiesce_core`;
  - `ave_pm_prepare`, `ave_pm_post`, `ave_pm_work`, `ave_pm_notify`,
    `ave_pm_suspend`;
  - the notifier registered at the end of `ave_probe` and unregistered first
    in `ave_remove`.
- `driver/ave_dapf.c`: `ave_dapf_capture` (stage 8, `pm_sleep=2` only, reads
  only) and `ave_dapf_pm_restore`.
- `driver/ave_v4l2.c`: `ave_v4l2_pm_quiesce` (returns holding `hw_mutex`),
  `_pm_lock`, `_pm_unlock`, `_pm_resume`, `ave_v4l2_idle`, and `ctx->pm_lost`.
- `driver/ave_session.c`: `ave_enc_start` returns EBUSY and `ave_enc_encode`
  returns EIO unless the state is ON.
- `driver/ave_smmu.c`: `ave_smmu_init` re-arms instead of requesting its IRQ
  a second time.
- `driver/ave_ipc.c`: `ave_ipc_init` initialises `ipc_lock` only once.
