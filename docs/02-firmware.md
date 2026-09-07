# The AVE firmware image

## Where it ships

Unlike AVD — whose firmware Apple embeds inside the kext along with per-SoC
config data — AVE firmware is a **standalone Image4 payload** in the restore
image:

```
Firmware/ave/AppleAVE2FW_H13C.im4p    2622961 bytes
Firmware/ave/AppleAVE2FW_H13D.im4p    2622961
Firmware/ave/AppleAVE2FW_H13G.im4p    2417841
Firmware/ave/AppleAVE2FW_H13S.im4p    2453177
... H14*, H15*, H16*, H17*
```

One variant per SoC. The payload is LZFSE-compressed inside the Image4
container and unwraps with `pyimg4`.

### Which variant is M1 Max?

**Inferred, not confirmed: `H13C`.** The reasoning is that `H13C` and `H13D`
are byte-identical in size, which fits M1 Ultra being two M1 Max dies, leaving
`G` as the base part and `S` as Pro. The firmware itself contains
`CAVCController_H13C.cpp` and `CHEVCController_H13C.cpp`, confirming the
variant suffix is a per-SoC discriminator, but not which chip maps to which
letter.

**This must be confirmed against `AppleAVE2.kext`'s selection logic before any
driver depends on it.** Loading the wrong variant is a plausible early
bring-up failure that would be hard to diagnose.

### Not currently extracted by Asahi

`/lib/firmware/vendor/apple/` on an installed Asahi system contains ISP and AOP
data only. Nothing collects AVE firmware today, and the Asahi documentation
notes the ANE/AVE/ADT `im4p`s do not extract with the standard tool. Adding
AVE to `asahi-fwextract` is a small, self-contained first contribution that is
useful before any driver exists.

## Format

```
Mach-O 64-bit arm64e (caps: PAC00) preload executable, flags:<NOUNDEFS|PIE>
cputype 0x0100000c  cpusubtype 0x80000002  filetype 5 (MH_PRELOAD)  ncmds 7
```

This is **not** a Cortex-M3 image. It is a full ARM64e application-class
binary with pointer authentication, loading at VA 0. That places AVE in the
same family as DCP and ISP — an ASC coprocessor — rather than with AVD's bare
Cortex-M3.

Segments (`tools/macho_info.py`):

```
__TEXT        vm 0x000000+0x134000    __text 0xfd710, __cstring 0x14587
__DATA        vm 0x134000+0x134000
  _rtk_patchbay, _rtk_mtab, _rtk_power, _rtk_tunables,
  _rtk_init_stack (64K), _rtk_irq_stack (4K), _rtk_exc_stack (4K),
  _rtk_boot (32K), _rtk_page_tables (256K), _rtk_heap, _rtk_threads
__DATA_CONST  vm 0x268000+0
```

The `_rtk_*` section names are RTKit's standard layout.

## It is RTKit

Decisive, from the image's own strings and symbols:

```
RTKit-3255.160.4.release
_RTK_mbi_endpoint_attach / _RTK_mbi_endpoint_enable_all / _RTK_mbi_endpoint_send_msg
_RTK_dev_mailbox128e_dispatch
_RTK_mbi_route_create / _RTK_mbi_route_enable / _RTK_mbi_log_create
RTK_crashlog_init, _RTK_tracekit_tracelist, RTK_platform_crashlog_complete
./RTKit/ffw/CMailboxPool.cpp
```

`mailbox128e` is the standard Apple ASC 128-bit mailbox. The channel layer is
also stock Apple: `PlatformIOPIPCManager`, `CChannelManager`, `CRealChannel` /
`CFakeChannel`, `ffwIOPChannelDescriptor`, plus `CSharedMemory`,
`CSharedMemoryHost` and `CSharedMemoryHeap`.

**Consequence:** m1n1's RTKit tracer and Linux's `apple-rtkit` apply directly.
The transport does not have to be built from scratch.

### Correction to an earlier assumption

An initial reading of the ADT suggested AVE was *not* RTKit, because `ave0`
lacks the `iop,ascwrap-v4` compatible string that `sio` and `aop` carry, and
has no `iop-*-nub` child node. **That inference was wrong.** Absence of
`ascwrap` is not evidence against RTKit; `ane0`, `isp0` and `avd0` likewise
lack it. Only the firmware image settles the question. Recorded here so the
mistake is not repeated.

## Endpoint ids are not statically recoverable

`__rtk_mbi_max_route = 8`, and `__mbi_routes` (at `0x1348e0`) is a descriptor
holding that count plus a pointer to `__rtk_mbi_routes_array` (`0x1348a0`),
an 8-entry array that is **zero in the shipped image**.

Disassembling `PlatformIOPIPCManager::InitMailboxRoute` (`0xe66a4`) shows a
loop over channel descriptors making virtual calls, driven by tables in BSS
(`_RTK_platform_mbi_config` at `0x2649c0`, `_RTK_platform_mbi_config_sve` at
`0x265a90`, strides `0x108` and `0x38`). Those are populated at runtime.

So: **AVE uses standard RTKit MBI with up to 8 routes, but the actual endpoint
numbers must come from a hypervisor trace or from deeper disassembly of the
init path.** This is the boundary of what static analysis gives, and the first
thing tracing should answer.
