# The firmware's I/O base is zero in the image we load

*Why the coprocessor starts, changes state, and then says nothing — and why
none of the boot-handshake work could have fixed it on its own.*

Found by an adversarial review on 2026-09-08, after the boot-handshake work in
[34](34-boot-handshake.md) was already implemented and about to be tested.

---

## 1. The firmware does not hard-code its register base

`CPlatformEnvironment`'s constructor calls `0xe18c0`, which copies a
length-prefixed tag out of the image's own `__DATA` into a global and then
forms every MMIO address from it:

```
 e18dc:  add  x8, x8, #0x1f5     ; __DATA 0x1341f5, the "IOBA" tag
 e18e4:  add  x19, x19, #0x9b8   ; dst = 0x2649b8
 e18e8:  ldrb w9, [x8, #4]!      ; length word at 0x1341f9  (= 8)
 e190c:  add  x1, x8, #0x4       ; payload at 0x1341fd
 e1910:  bl   0x581c             ; memcpy(0x2649b8, payload, 8)
 e1914:  ldr  x8, [x19]          ; base
 e1920:  add  x9, x8, #0x1800000 ; -> the ASC bank
 e1934:  add  x1, x8, #0x1c00000 ; -> the CPU-control window
 e1a78:  str  wzr, [x8, x9]      ; first MMIO write: base + 0x1c00808
```

**Confirmed.** So the base is data, not code, and it is supplied per boot.

## 2. In our image that data is zero

```
$ python3 -c "d=open('data/blobs/ave_h13c.bin','rb').read(); \
             o=0x1341f5+0x4000; print(d[o:o+32].hex(' '))"
41 42 4f 49 08 00 00 00 00 00 00 00 00 00 00 00
5a 53 4f 49 04 00 00 00 00 00 00 00 00 00 00 00
```

`41 42 4f 49` is `"IOBA"` byte-reversed, length 8, payload **eight zero
bytes**; `5a 53 4f 49` is `"IOSZ"`, length 4, payload zero. **Confirmed.**

On an Apple boot iBoot fills these in before handing the image over. Nothing on
our path does: the tag blob has three readers in the whole text segment
(`0x31b4c`, `0xe18dc`, `0xfba78`) and no writer, the kext contains neither the
`IOBA` string nor the immediate `0x494f4241`, and per
[09](09-firmware-load.md) §1.6 the kext cannot load an image at all.

## 3. Therefore

With a base of zero the coprocessor's first register write goes to bus
`0x01c00808` and every scratch and IPI access lands somewhere in `0x0105xxxx`.
It never touches the SVE block no matter what we put in the boot-config block.
The core starts, executes, addresses nothing that exists, and is silent —
which is exactly the symptom recorded in [31](31-bringup-state.md).

**This is hypothesis 0**, ahead of stream IDs, power domains and cache
attributes. It also means the boot handshake in
[34](34-boot-handshake.md) could not have been tested on its own: correct as
that work is, the firmware could not have reached the registers it describes.

## 4. The value

The tag takes a **bus** address — the coprocessor sits on the far side of the
`/arm-io` translation — so it is `0x20C000000`, *not* the AP-physical
`0x40C000000`. This is [30](30-address-translation-bug.md) in reverse, and
worth the care: that confusion in the other direction cost eight experiments.

It cross-checks two ways. `0x20C000000 + 0x1800000 = 0x20D800000` is the ASC
bank, and `0x20C000000 + 0x1050000 = 0x20D050000` is the SVE bank — both the
bus forms of the banks in `ave_hw.h`. And it agrees with the independent
derivation in [34](34-boot-handshake.md) §1.

`driver/ave_fw.c` therefore derives it at load time as
`bank[AVE_BANK_FABRIC].phys - AVE_ARM_IO_BUS_OFFSET` rather than hard-coding
it, so the two address spaces cannot drift apart again, and patches the tag in
the copied image before it is mapped. `IOSZ` gets `0x2000000` (32 MiB).

## 5. What this does not explain

Nothing here says the handshake, the DART mapping or the power sequence are
right — only that they could not have been *observed* to be right. Every
earlier negative result about the firmware's silence is uninformative rather
than wrong, and none of them should be cited as evidence against any other
hypothesis.
