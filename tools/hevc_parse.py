#!/usr/bin/env python3
"""Read the VPS, SPS, PPS and slice headers out of an Annex B HEVC stream.

    tools/hevc_parse.py results/<run>-load1/paramsets.bin      # H1: HEVC_INIT only
    tools/hevc_parse.py results/<run>-load1/frame.h265         # H2/H3: whole stream
    tools/hevc_parse.py --check [--size 1280x720] [--qp 30] FILE

The tools/h264_parse.py counterpart for docs/77. What the firmware wrote is
evidence from the encoder: profile, level, picture size, CTB size, SAO, WPP,
the scaling-list flags, and per slice the NAL type, slice type, POC lsb and
slice QP - none of it needs the hardware to read again.

Works on the param-sets blob alone (VPS/SPS/PPS, 4-byte start codes), on
frame.h265 (param sets, then per slice the firmware's slice header followed
by the coded bytes), and on a raw SliceHeader slot (slicehdr.bin) when the
param sets are given first:  hevc_parse.py paramsets.bin slicehdr.bin

--check grades the docs/77 §8.2 H1 expectations and exits non-zero on a
miss: NAL types 32, 33, 34 in that order; Main profile; CTB 32, min CB 8,
TB 4..32; scaling_list_enabled 0 (flat, docs/77 §2.5); optionally the
display size and every slice's QP.

Deliberately small: single layer, no VUI/HRD decoding (reported and
skipped), no long-term refs, no weighted prediction, no list modification -
where the stream needs one of those, the parser says so and stops that
header rather than guess.
"""
import argparse
import sys

NAL_NAMES = {
    0: "TRAIL_N", 1: "TRAIL_R", 2: "TSA_N", 3: "TSA_R", 4: "STSA_N",
    5: "STSA_R", 6: "RADL_N", 7: "RADL_R", 8: "RASL_N", 9: "RASL_R",
    16: "BLA_W_LP", 17: "BLA_W_RADL", 18: "BLA_N_LP", 19: "IDR_W_RADL",
    20: "IDR_N_LP", 21: "CRA_NUT", 32: "VPS", 33: "SPS", 34: "PPS",
    35: "AUD", 36: "EOS", 37: "EOB", 38: "FD", 39: "SEI_PREFIX",
    40: "SEI_SUFFIX",
}
SLICE_TYPES = {0: "B", 1: "P", 2: "I"}


class Unsupported(Exception):
    pass


def nal_units(buf: bytes):
    """Annex B -> (offset, nal_unit_type, layer, tid, RBSP payload after the 2-byte header)."""
    i, n = 0, len(buf)
    starts = []
    while i < n - 2:
        if buf[i] == 0 and buf[i + 1] == 0 and buf[i + 2] == 1:
            starts.append(i + 3)
            i += 3
        else:
            i += 1
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else n
        while e > s and buf[e - 1] == 0:
            e -= 1
        if e - s < 2:
            continue
        h = (buf[s] << 8) | buf[s + 1]
        raw, j = bytearray(), s + 2
        while j < e:
            if j + 2 < e and buf[j] == 0 and buf[j + 1] == 0 and buf[j + 2] == 3:
                raw += b"\x00\x00"
                j += 3
            else:
                raw.append(buf[j])
                j += 1
        yield (s - 4 if s >= 4 and buf[s - 4] == 0 else s - 3,
               (h >> 9) & 0x3F, (h >> 3) & 0x3F, (h & 7) - 1, bytes(raw),
               h >> 15)


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
        while self.u(1) == 0:
            z += 1
            if z > 31:
                raise ValueError("bad Exp-Golomb code at bit %d" % self.p)
        return (1 << z) - 1 + self.u(z)

    def se(self) -> int:
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)

    def left(self) -> int:
        return len(self.d) * 8 - self.p


def ceil_log2(v: int) -> int:
    n = 0
    while (1 << n) < v:
        n += 1
    return n


def profile_tier_level(b: Bits, max_sub: int) -> dict:
    p = {"profile_space": b.u(2), "tier": b.u(1), "profile_idc": b.u(5)}
    p["compat"] = [b.u(1) for _ in range(32)]
    p["progressive"], p["interlaced"] = b.u(1), b.u(1)
    p["non_packed"], p["frame_only"] = b.u(1), b.u(1)
    b.u(32)
    b.u(11)                     # 43 reserved bits
    b.u(1)                      # general_inbld / reserved
    p["level_idc"] = b.u(8)
    sub_prof, sub_lvl = [], []
    for _ in range(max_sub):
        sub_prof.append(b.u(1))
        sub_lvl.append(b.u(1))
    if max_sub > 0:
        for _ in range(max_sub, 8):
            b.u(2)
    for i in range(max_sub):
        if sub_prof[i]:
            b.u(32)
            b.u(32)
            b.u(24)             # 88 bits
        if sub_lvl[i]:
            b.u(8)
    return p


def scaling_list_data(b: Bits) -> dict:
    out = {"pred_mode": [], "dc": []}
    for size_id in range(4):
        step = 3 if size_id == 3 else 1
        for _ in range(0, 6, step):
            pred = b.u(1)
            out["pred_mode"].append(pred)
            if not pred:
                b.ue()          # scaling_list_pred_matrix_id_delta
            else:
                coefs = min(64, 1 << (4 + (size_id << 1)))
                if size_id > 1:
                    out["dc"].append(b.se() + 8)
                for _ in range(coefs):
                    b.se()
    return out


def st_ref_pic_set(b: Bits, idx: int, num_sets: int, sets: list) -> dict:
    inter = b.u(1) if idx != 0 else 0
    if inter:
        delta_idx = b.ue() + 1 if idx == num_sets else 1
        ref = sets[idx - delta_idx]
        sign, abs_delta = b.u(1), b.ue() + 1
        delta_rps = (1 - 2 * sign) * abs_delta
        used, use_delta = [], []
        for _ in range(ref["num_delta_pocs"] + 1):
            u_ = b.u(1)
            d_ = b.u(1) if not u_ else 1
            used.append(u_)
            use_delta.append(d_)
        # Derive (7-61/7-62) enough to know NumDeltaPocs for later sets.
        s0, s1 = [], []
        rs0, rs1 = ref["s0"], ref["s1"]
        for j in range(len(rs1) - 1, -1, -1):
            dp = rs1[j][0] + delta_rps
            if dp < 0 and use_delta[len(rs0) + j]:
                s0.append((dp, used[len(rs0) + j]))
        if delta_rps < 0 and use_delta[ref["num_delta_pocs"]]:
            s0.append((delta_rps, used[ref["num_delta_pocs"]]))
        for j in range(len(rs0)):
            dp = rs0[j][0] + delta_rps
            if dp < 0 and use_delta[j]:
                s0.append((dp, used[j]))
        for j in range(len(rs0) - 1, -1, -1):
            dp = rs0[j][0] + delta_rps
            if dp > 0 and use_delta[j]:
                s1.append((dp, used[j]))
        if delta_rps > 0 and use_delta[ref["num_delta_pocs"]]:
            s1.append((delta_rps, used[ref["num_delta_pocs"]]))
        for j in range(len(rs1)):
            dp = rs1[j][0] + delta_rps
            if dp > 0 and use_delta[len(rs0) + j]:
                s1.append((dp, used[len(rs0) + j]))
    else:
        nneg, npos = b.ue(), b.ue()
        s0, s1, poc = [], [], 0
        for _ in range(nneg):
            poc -= b.ue() + 1
            s0.append((poc, b.u(1)))
        poc = 0
        for _ in range(npos):
            poc += b.ue() + 1
            s1.append((poc, b.u(1)))
    return {"inter": inter, "s0": s0, "s1": s1, "num_delta_pocs": len(s0) + len(s1)}


def parse_vps(b: Bits) -> dict:
    v = {"id": b.u(4), "base_internal": b.u(1), "base_available": b.u(1),
         "max_layers_m1": b.u(6), "max_sub_layers_m1": b.u(3),
         "temporal_nesting": b.u(1)}
    v["reserved_ffff"] = b.u(16)
    v["ptl"] = profile_tier_level(b, v["max_sub_layers_m1"])
    info = b.u(1)
    v["sub_layer_ordering_info"] = info
    v["max_dec_pic_buffering_m1"] = []
    for _ in range(0 if info else v["max_sub_layers_m1"], v["max_sub_layers_m1"] + 1):
        v["max_dec_pic_buffering_m1"].append(b.ue())
        b.ue()
        b.ue()
    v["max_layer_id"] = b.u(6)
    v["num_layer_sets_m1"] = b.ue()
    for _ in range(v["num_layer_sets_m1"]):
        b.u(v["max_layer_id"] + 1)
    v["timing_info"] = b.u(1)
    if not v["timing_info"]:
        v["extension"] = b.u(1)
    return v


def parse_sps(b: Bits) -> dict:
    s = {"vps_id": b.u(4), "max_sub_layers_m1": b.u(3), "temporal_nesting": b.u(1)}
    s["ptl"] = profile_tier_level(b, s["max_sub_layers_m1"])
    s["id"] = b.ue()
    s["chroma_format_idc"] = b.ue()
    s["separate_colour_plane"] = b.u(1) if s["chroma_format_idc"] == 3 else 0
    s["width"], s["height"] = b.ue(), b.ue()
    s["conf_win"] = [b.ue() for _ in range(4)] if b.u(1) else [0, 0, 0, 0]
    s["bit_depth_luma"], s["bit_depth_chroma"] = b.ue() + 8, b.ue() + 8
    s["log2_max_poc_lsb"] = b.ue() + 4
    info = b.u(1)
    s["sub_layer_ordering_info"] = info
    s["max_dec_pic_buffering_m1"], s["num_reorder"] = [], []
    for _ in range(0 if info else s["max_sub_layers_m1"], s["max_sub_layers_m1"] + 1):
        s["max_dec_pic_buffering_m1"].append(b.ue())
        s["num_reorder"].append(b.ue())
        b.ue()
    s["log2_min_cb"] = b.ue() + 3
    s["log2_ctb"] = s["log2_min_cb"] + b.ue()
    s["log2_min_tb"] = b.ue() + 2
    s["log2_max_tb"] = s["log2_min_tb"] + b.ue()
    s["tb_depth_inter"], s["tb_depth_intra"] = b.ue(), b.ue()
    s["scaling_list_enabled"] = b.u(1)
    s["scaling_list_data_present"] = 0
    if s["scaling_list_enabled"]:
        s["scaling_list_data_present"] = b.u(1)
        if s["scaling_list_data_present"]:
            s["scaling_list"] = scaling_list_data(b)
    s["amp"], s["sao"], s["pcm"] = b.u(1), b.u(1), b.u(1)
    if s["pcm"]:
        b.u(4)
        b.u(4)
        b.ue()
        b.ue()
        b.u(1)
    n = b.ue()
    s["st_rps"] = []
    for i in range(n):
        s["st_rps"].append(st_ref_pic_set(b, i, n, s["st_rps"]))
    s["long_term_refs"] = b.u(1)
    if s["long_term_refs"]:
        k = b.ue()
        for _ in range(k):
            b.u(s["log2_max_poc_lsb"])
            b.u(1)
    s["tmvp"] = b.u(1)
    s["strong_intra_smoothing"] = b.u(1)
    s["vui"] = b.u(1)
    ctb = 1 << s["log2_ctb"]
    s["ctb_cols"] = -(-s["width"] // ctb)
    s["ctb_rows"] = -(-s["height"] // ctb)
    sub_w = 2 if s["chroma_format_idc"] in (1, 2) else 1
    sub_h = 2 if s["chroma_format_idc"] == 1 else 1
    l, r, t, bt = s["conf_win"]
    s["display"] = (s["width"] - sub_w * (l + r), s["height"] - sub_h * (t + bt))
    return s


def parse_pps(b: Bits) -> dict:
    p = {"id": b.ue(), "sps_id": b.ue(), "dependent_slices": b.u(1),
         "output_flag_present": b.u(1), "extra_sh_bits": b.u(3),
         "sign_data_hiding": b.u(1), "cabac_init_present": b.u(1),
         "num_ref_l0_default_m1": b.ue(), "num_ref_l1_default_m1": b.ue(),
         "init_qp_m26": b.se(), "constrained_intra": b.u(1),
         "transform_skip": b.u(1), "cu_qp_delta": b.u(1)}
    p["cu_qp_delta_depth"] = b.ue() if p["cu_qp_delta"] else 0
    p["cb_qp_offset"], p["cr_qp_offset"] = b.se(), b.se()
    p["slice_chroma_qp_offsets_present"] = b.u(1)
    p["weighted_pred"], p["weighted_bipred"] = b.u(1), b.u(1)
    p["transquant_bypass"], p["tiles"], p["wpp"] = b.u(1), b.u(1), b.u(1)
    if p["tiles"]:
        cols, rows = b.ue(), b.ue()
        if not b.u(1):
            for _ in range(cols):
                b.ue()
            for _ in range(rows):
                b.ue()
        b.u(1)
    p["lf_across_slices"] = b.u(1)
    p["deblock_ctrl_present"] = b.u(1)
    p["deblock_override_enabled"] = p["deblock_disabled"] = 0
    p["beta_offset_div2"] = p["tc_offset_div2"] = 0
    if p["deblock_ctrl_present"]:
        p["deblock_override_enabled"] = b.u(1)
        p["deblock_disabled"] = b.u(1)
        if not p["deblock_disabled"]:
            p["beta_offset_div2"], p["tc_offset_div2"] = b.se(), b.se()
    p["scaling_list_data_present"] = b.u(1)
    if p["scaling_list_data_present"]:
        scaling_list_data(b)
    p["lists_modification"] = b.u(1)
    p["log2_parallel_merge_level"] = b.ue() + 2
    p["sh_extension"] = b.u(1)
    p["extension"] = b.u(1)
    return p


def parse_slice(b: Bits, nut: int, spss: dict, ppss: dict) -> dict:
    sl = {"first": b.u(1)}
    if 16 <= nut <= 23:
        sl["no_output_of_prior_pics"] = b.u(1)
    sl["pps_id"] = b.ue()
    pps = ppss.get(sl["pps_id"])
    if pps is None:
        raise Unsupported("slice names PPS %d, which was not seen" % sl["pps_id"])
    sps = spss.get(pps["sps_id"])
    if sps is None:
        raise Unsupported("PPS names SPS %d, which was not seen" % pps["sps_id"])
    dependent = 0
    if not sl["first"]:
        if pps["dependent_slices"]:
            dependent = b.u(1)
        sl["address"] = b.u(ceil_log2(sps["ctb_cols"] * sps["ctb_rows"]))
    sl["dependent"] = dependent
    if dependent:
        return sl
    for _ in range(pps["extra_sh_bits"]):
        b.u(1)
    sl["type"] = b.ue()
    if pps["output_flag_present"]:
        b.u(1)
    if sps["separate_colour_plane"]:
        b.u(2)
    sl["poc_lsb"] = 0
    sl["tmvp"] = 0
    num_pic_total_curr = 0
    if nut not in (19, 20):
        sl["poc_lsb"] = b.u(sps["log2_max_poc_lsb"])
        sl["st_rps_sps"] = b.u(1)
        if not sl["st_rps_sps"]:
            rps = st_ref_pic_set(b, len(sps["st_rps"]), len(sps["st_rps"]), sps["st_rps"])
        else:
            idx = b.u(ceil_log2(len(sps["st_rps"]))) if len(sps["st_rps"]) > 1 else 0
            sl["st_rps_idx"] = idx
            rps = sps["st_rps"][idx] if idx < len(sps["st_rps"]) else None
            if rps is None:
                raise Unsupported("short-term RPS index %d past the SPS's %d sets"
                                  % (idx, len(sps["st_rps"])))
        sl["rps"] = rps
        num_pic_total_curr = sum(u for _, u in rps["s0"] + rps["s1"])
        if sps["long_term_refs"]:
            raise Unsupported("long-term reference pictures")
        if sps["tmvp"]:
            sl["tmvp"] = b.u(1)
    sl["sao_luma"] = sl["sao_chroma"] = 0
    if sps["sao"]:
        sl["sao_luma"] = b.u(1)
        if sps["chroma_format_idc"] != 0 and not sps["separate_colour_plane"]:
            sl["sao_chroma"] = b.u(1)
    if sl["type"] in (0, 1):
        sl["num_ref_l0"] = pps["num_ref_l0_default_m1"] + 1
        sl["num_ref_l1"] = pps["num_ref_l1_default_m1"] + 1
        sl["ref_override"] = b.u(1)
        if sl["ref_override"]:
            sl["num_ref_l0"] = b.ue() + 1
            if sl["type"] == 0:
                sl["num_ref_l1"] = b.ue() + 1
        if pps["lists_modification"] and num_pic_total_curr > 1:
            raise Unsupported("ref_pic_lists_modification")
        if sl["type"] == 0:
            b.u(1)              # mvd_l1_zero_flag
        if pps["cabac_init_present"]:
            b.u(1)
        if sl["tmvp"]:
            col_l0 = b.u(1) if sl["type"] == 0 else 1
            if (col_l0 and sl["num_ref_l0"] > 1) or \
               (not col_l0 and sl.get("num_ref_l1", 0) > 1):
                b.ue()
        if (pps["weighted_pred"] and sl["type"] == 1) or \
           (pps["weighted_bipred"] and sl["type"] == 0):
            raise Unsupported("pred_weight_table")
        sl["max_merge_cand"] = 5 - b.ue()
    sl["qp_delta"] = b.se()
    sl["qp"] = 26 + pps["init_qp_m26"] + sl["qp_delta"]
    if pps["slice_chroma_qp_offsets_present"]:
        b.se()
        b.se()
    override = 0
    if pps["deblock_override_enabled"]:
        override = b.u(1)
    sl["deblock_disabled"] = pps["deblock_disabled"]
    if override:
        sl["deblock_disabled"] = b.u(1)
        if not sl["deblock_disabled"]:
            b.se()
            b.se()
    sl["lf_across"] = pps["lf_across_slices"]
    if pps["lf_across_slices"] and (sl["sao_luma"] or sl["sao_chroma"] or
                                    not sl["deblock_disabled"]):
        sl["lf_across"] = b.u(1)
    sl["entry_points"] = 0
    if pps["tiles"] or pps["wpp"]:
        sl["entry_points"] = b.ue()
        if sl["entry_points"]:
            n = b.ue() + 1
            sl["entry_point_offsets"] = [b.u(n) + 1 for _ in range(sl["entry_points"])]
    if pps["sh_extension"]:
        for _ in range(b.ue()):
            b.u(8)
    # byte_alignment(): a 1 then zeros to the byte boundary.
    one = b.u(1)
    pad = b.u((8 - b.p % 8) % 8)
    sl["byte_alignment_ok"] = one == 1 and pad == 0
    sl["header_bits"] = b.p
    return sl


def fmt_level(idc: int) -> str:
    return "%d.%d" % (idc // 30, (idc % 30) // 3)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="+", help="Annex B inputs, read as one stream in order")
    ap.add_argument("--check", action="store_true",
                    help="grade the docs/77 §8.2 H1 expectations; non-zero exit on a miss")
    ap.add_argument("--size", help="expected display size WxH (with --check)")
    ap.add_argument("--qp", type=int, help="expected slice QP (with --check)")
    args = ap.parse_args()

    buf = b"".join(open(f, "rb").read() for f in args.files)
    vpss, spss, ppss = {}, {}, {}
    order, slices, problems = [], [], []

    for off, nut, layer, tid, rbsp, forbidden in nal_units(buf):
        name = NAL_NAMES.get(nut, "type %d" % nut)
        order.append(nut)
        b = Bits(rbsp)
        head = "@%-6d %-10s (type %2d, layer %d, tid %d%s)" % (
            off, name, nut, layer, tid, ", FORBIDDEN BIT SET" if forbidden else "")
        try:
            if nut == 32:
                v = parse_vps(b)
                vpss[v["id"]] = v
                p = v["ptl"]
                print("%s id %d, max_layers %d, max_sub_layers %d, nesting %d, "
                      "0xffff field %#x, profile %d, level %s (idc %d), "
                      "max_dec_pic_buffering_minus1 %s, timing %d"
                      % (head, v["id"], v["max_layers_m1"] + 1,
                         v["max_sub_layers_m1"] + 1, v["temporal_nesting"],
                         v["reserved_ffff"], p["profile_idc"],
                         fmt_level(p["level_idc"]), p["level_idc"],
                         v["max_dec_pic_buffering_m1"], v["timing_info"]))
            elif nut == 33:
                s = parse_sps(b)
                spss[s["id"]] = s
                p = s["ptl"]
                compat = [j for j, f in enumerate(p["compat"]) if f]
                print("%s id %d, profile %d (compat %s) tier %d, level %s (idc %d)"
                      % (head, s["id"], p["profile_idc"], compat, p["tier"],
                         fmt_level(p["level_idc"]), p["level_idc"]))
                print("         %dx%d coded, conf_win l/r/t/b %s -> %dx%d display, "
                      "chroma_format %d, %d-bit, nesting %d"
                      % (s["width"], s["height"], s["conf_win"], s["display"][0],
                         s["display"][1], s["chroma_format_idc"],
                         s["bit_depth_luma"], s["temporal_nesting"]))
                print("         CTB %d (%dx%d CTBs), min CB %d, TB %d..%d, depth inter %d "
                      "intra %d, POC lsb %d bits, max_dec_pic_buffering_minus1 %s"
                      % (1 << s["log2_ctb"], s["ctb_cols"], s["ctb_rows"],
                         1 << s["log2_min_cb"], 1 << s["log2_min_tb"],
                         1 << s["log2_max_tb"], s["tb_depth_inter"],
                         s["tb_depth_intra"], s["log2_max_poc_lsb"],
                         s["max_dec_pic_buffering_m1"]))
                print("         scaling_list_enabled %d data_present %d, AMP %d, SAO %d, "
                      "PCM %d, TMVP %d, strong_intra %d, VUI %d%s"
                      % (s["scaling_list_enabled"], s["scaling_list_data_present"],
                         s["amp"], s["sao"], s["pcm"], s["tmvp"],
                         s["strong_intra_smoothing"], s["vui"],
                         " (not decoded)" if s["vui"] else ""))
                for i, r in enumerate(s["st_rps"]):
                    print("         st_rps[%d]: S0 %s S1 %s (delta POC, used)"
                          % (i, r["s0"], r["s1"]))
            elif nut == 34:
                p = parse_pps(b)
                ppss[p["id"]] = p
                print("%s id %d -> SPS %d, init_qp %d, cu_qp_delta %d (depth %d), "
                      "WPP %d, tiles %d, sign_hiding %d, cabac_init %d, "
                      "extra_sh_bits %d, transquant_bypass %d, transform_skip %d"
                      % (head, p["id"], p["sps_id"], 26 + p["init_qp_m26"],
                         p["cu_qp_delta"], p["cu_qp_delta_depth"], p["wpp"],
                         p["tiles"], p["sign_data_hiding"],
                         p["cabac_init_present"], p["extra_sh_bits"],
                         p["transquant_bypass"], p["transform_skip"]))
                print("         deblocking ctrl %d override %d disabled %d, "
                      "lf_across_slices %d, scaling_data %d, lists_mod %d, "
                      "par_merge %d, sh_ext %d"
                      % (p["deblock_ctrl_present"], p["deblock_override_enabled"],
                         p["deblock_disabled"], p["lf_across_slices"],
                         p["scaling_list_data_present"], p["lists_modification"],
                         p["log2_parallel_merge_level"], p["sh_extension"]))
            elif nut <= 31:
                sl = parse_slice(b, nut, spss, ppss)
                slices.append(sl)
                if sl.get("dependent"):
                    print("%s dependent slice segment" % head)
                    continue
                ref = ""
                if "rps" in sl:
                    ref = ", RPS S0 %s S1 %s%s" % (
                        sl["rps"]["s0"], sl["rps"]["s1"],
                        " (SPS set %d)" % sl["st_rps_idx"] if sl.get("st_rps_sps") else " (explicit)")
                print("%s %s slice, first %d, POC lsb %d, slice_qp %d (delta %+d), "
                      "SAO %d/%d, TMVP %d, entry points %d, deblock off %d, "
                      "header %d bits (byte_alignment %s)%s%s"
                      % (head, SLICE_TYPES.get(sl["type"], "?"), sl["first"],
                         sl["poc_lsb"], sl["qp"], sl["qp_delta"], sl["sao_luma"],
                         sl["sao_chroma"], sl["tmvp"], sl["entry_points"],
                         sl["deblock_disabled"], sl["header_bits"] + 16,
                         "ok" if sl["byte_alignment_ok"] else "BAD",
                         ", refs l0 %d" % sl["num_ref_l0"] if "num_ref_l0" in sl else "",
                         ref))
            else:
                print("%s %d bytes" % (head, len(rbsp)))
        except Unsupported as e:
            print("%s not parsed: %s" % (head, e))
            problems.append("%s: %s" % (name, e))
        except (ValueError, IndexError, KeyError) as e:
            print("%s PARSE ERROR: %s" % (head, e))
            problems.append("%s: parse error %s" % (name, e))

    if not order:
        print("no NAL units found")
        return 1
    if not args.check:
        return 0

    fails = []

    def expect(ok, what):
        print("  %s %s" % ("ok  " if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    print("check (docs/77 §8.2 H1):")
    expect(order[:3] == [32, 33, 34], "first three NALs are VPS, SPS, PPS (got %s)" % order[:3])
    expect(len(spss) == 1 and len(ppss) == 1, "exactly one SPS and one PPS")
    for s in spss.values():
        expect(s["ptl"]["profile_idc"] == 1, "Main profile (general_profile_idc 1)")
        expect(s["log2_ctb"] == 5, "CTB 32 (the pipe's, docs/77 §0 #6)")
        expect(s["log2_min_cb"] == 3, "min CB 8")
        expect((s["log2_min_tb"], s["log2_max_tb"]) == (2, 5), "TB 4..32")
        expect(s["scaling_list_enabled"] == 0,
               "scaling_list_enabled 0 (PICMGMT+0x6F4 = 0 is flat, docs/77 §2.5)")
        expect(s["chroma_format_idc"] == 1 and s["bit_depth_luma"] == 8, "8-bit 4:2:0")
        expect(s["max_sub_layers_m1"] > 0 or s["temporal_nesting"] == 1,
               "temporal_id_nesting 1 with one sub-layer")
        if args.size:
            w, h = (int(x) for x in args.size.lower().split("x"))
            expect(s["display"] == (w, h), "display %dx%d (got %dx%d)"
                   % (w, h, s["display"][0], s["display"][1]))
    for v in vpss.values():
        expect(v["reserved_ffff"] == 0xFFFF, "VPS reserved 0xffff field")
    for p in ppss.values():
        expect(p["extra_sh_bits"] == 0, "num_extra_slice_header_bits 0")
    for i, sl in enumerate(slices):
        if "qp" in sl:
            expect(sl["byte_alignment_ok"], "slice %d header ends in byte_alignment()" % i)
            if args.qp is not None:
                expect(sl["qp"] == args.qp, "slice %d QP %d (got %d)" % (i, args.qp, sl["qp"]))
    for p in problems:
        expect(False, p)
    print("check: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
