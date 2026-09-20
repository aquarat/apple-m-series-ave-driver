#!/usr/bin/env python3
"""Read the SPS, PPS and slice headers out of an Annex B H.264 stream.

    tools/h264_parse.py results/<run>-load1/frame.h264

Written because a reboot was nearly spent re-reading a QP the bitstream
already states. The encoder's own output says what QP it coded at, what
slice type it chose and how big each slice is; none of that needs the
hardware, and getting it from the stream is evidence from the encoder rather
than inference from a register we programmed.

Deliberately small: enough syntax to reach slice_qp_delta for a baseline
I/P stream, and no further.
"""
import sys


def nal_units(buf: bytes):
    """Annex B start codes -> (nal_ref_idc, nal_unit_type, RBSP payload)."""
    i, n = 0, len(buf)
    starts = []
    while i < n - 3:
        if buf[i] == 0 and buf[i + 1] == 0 and buf[i + 2] == 1:
            starts.append(i + 3)
            i += 3
        else:
            i += 1
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else n
        while e > s and buf[e - 1] == 0:
            e -= 1
        if e <= s:
            continue
        hdr = buf[s]
        # Undo emulation prevention: 00 00 03 -> 00 00
        raw, j = bytearray(), s + 1
        while j < e:
            if j + 2 < e and buf[j] == 0 and buf[j + 1] == 0 and buf[j + 2] == 3:
                raw += b"\x00\x00"
                j += 3
            else:
                raw.append(buf[j])
                j += 1
        yield (hdr >> 5) & 3, hdr & 0x1F, bytes(raw)


class Bits:
    def __init__(self, data: bytes):
        self.d, self.p = data, 0

    def u(self, n: int) -> int:
        v = 0
        for _ in range(n):
            byte = self.d[self.p >> 3] if (self.p >> 3) < len(self.d) else 0
            v = (v << 1) | ((byte >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v

    def ue(self) -> int:
        z = 0
        while self.p < len(self.d) * 8 and self.u(1) == 0:
            z += 1
            if z > 32:
                return 0
        return (1 << z) - 1 + (self.u(z) if z else 0)

    def se(self) -> int:
        k = self.ue()
        return (k + 1) // 2 if k % 2 else -(k // 2)


def parse_sps(rbsp: bytes) -> dict:
    b = Bits(rbsp)
    s = {"profile_idc": b.u(8)}
    b.u(8)                                   # constraint flags + reserved
    s["level_idc"] = b.u(8)
    s["sps_id"] = b.ue()
    if s["profile_idc"] in (100, 110, 122, 244, 44, 83, 86, 118, 128):
        s["chroma_format_idc"] = b.ue()
        if s["chroma_format_idc"] == 3:
            b.u(1)
        b.ue(); b.ue(); b.u(1)
        if b.u(1):                           # seq_scaling_matrix_present
            return s                         # not needed for our streams
    s["log2_max_frame_num"] = b.ue() + 4
    s["poc_type"] = b.ue()
    if s["poc_type"] == 0:
        s["log2_max_poc_lsb"] = b.ue() + 4
    elif s["poc_type"] == 1:
        b.u(1); b.se(); b.se()
        for _ in range(b.ue()):
            b.se()
    s["max_num_ref_frames"] = b.ue()
    b.u(1)
    s["mbs_wide"] = b.ue() + 1
    s["map_units_high"] = b.ue() + 1
    s["frame_mbs_only"] = b.u(1)
    return s


def parse_pps(rbsp: bytes) -> dict:
    b = Bits(rbsp)
    p = {"pps_id": b.ue(), "sps_id": b.ue(), "entropy_coding_mode": b.u(1)}
    b.u(1)                                   # bottom_field_pic_order
    p["num_slice_groups"] = b.ue() + 1
    p["num_ref_idx_l0"] = b.ue() + 1
    p["num_ref_idx_l1"] = b.ue() + 1
    p["weighted_pred"] = b.u(1)
    p["weighted_bipred"] = b.u(2)
    p["pic_init_qp"] = b.se() + 26
    b.se(); b.se()
    return p


SLICE_TYPE = {0: "P", 1: "B", 2: "I", 3: "SP", 4: "SI",
              5: "P", 6: "B", 7: "I", 8: "SP", 9: "SI"}


def parse_slice(rbsp: bytes, nal_type: int, nal_ref_idc: int,
                sps: dict, pps: dict) -> dict:
    b = Bits(rbsp)
    sl = {"first_mb": b.ue()}
    st = b.ue()
    sl["slice_type"] = SLICE_TYPE.get(st, str(st))
    sl["pps_id"] = b.ue()
    sl["frame_num"] = b.u(sps.get("log2_max_frame_num", 4))
    if nal_type == 5:
        sl["idr_pic_id"] = b.ue()
    if sps.get("poc_type") == 0:
        sl["poc_lsb"] = b.u(sps.get("log2_max_poc_lsb", 4))
    if sl["slice_type"] in ("P", "SP"):
        if b.u(1):                           # num_ref_idx_active_override
            b.ue()
        if b.u(1):                           # ref_pic_list_modification
            while True:
                op = b.ue()
                if op == 3:
                    break
                b.ue()
    if nal_ref_idc:
        if nal_type == 5:
            sl["no_output_of_prior"] = b.u(1)
            sl["long_term_reference"] = b.u(1)
        elif b.u(1):                         # adaptive_ref_pic_marking
            while True:
                op = b.ue()
                if op == 0:
                    break
                if op in (1, 3):
                    b.ue()
                if op in (2,):
                    b.ue()
                if op in (3, 6):
                    b.ue()
                if op == 4:
                    b.ue()
                if op == 5:
                    break
    sl["slice_qp"] = pps["pic_init_qp"] + b.se()
    return sl


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    buf = open(sys.argv[1], "rb").read()
    sps, pps = {}, {}
    nslice = 0
    for ref_idc, typ, rbsp in nal_units(buf):
        if typ == 7:
            sps = parse_sps(rbsp)
            print(f"SPS: profile {sps['profile_idc']} level {sps['level_idc']} "
                  f"{sps['mbs_wide']}x{sps['map_units_high']} MBs "
                  f"({sps['mbs_wide'] * 16}x{sps['map_units_high'] * 16}), "
                  f"{sps['mbs_wide'] * sps['map_units_high']} total, "
                  f"max_num_ref_frames {sps.get('max_num_ref_frames')}")
        elif typ == 8:
            pps = parse_pps(rbsp)
            print(f"PPS: pic_init_qp {pps['pic_init_qp']} "
                  f"entropy_coding_mode {pps['entropy_coding_mode']} "
                  f"({'CABAC' if pps['entropy_coding_mode'] else 'CAVLC'}) "
                  f"slice_groups {pps['num_slice_groups']}")
        elif typ in (1, 5) and sps and pps:
            sl = parse_slice(rbsp, typ, ref_idc, sps, pps)
            nslice += 1
            print(f"slice {nslice}: {sl['slice_type']}"
                  f"{' IDR' if typ == 5 else ''} first_mb {sl['first_mb']} "
                  f"frame_num {sl['frame_num']} nal_ref_idc {ref_idc} "
                  f"**slice_qp {sl['slice_qp']}** ({len(rbsp)} RBSP bytes)")
    if not nslice:
        print("no slice NAL found")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
