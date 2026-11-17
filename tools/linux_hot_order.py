#!/usr/bin/env python3
"""
Generate a GNU ld --section-ordering-file script (linux_hot_order.ld) for
Linux/R36S aarch64 builds.

Packs the critical hot loop first (Zone A, targeting Cortex-A35 I-cache/TLB):
  1. Hot runtime & native symbols (Renderer, sotc_native, SoundMixer,
     PS2Memory, EEScheduler, decomp_*, pose82, vu1rec) from vita_hot_order.ld
  2. Top 400 trusted-float lean functions (leant_*) from game/lean_hot.txt
followed by Zone B (remaining native overrides, remaining leant_*, warm sub_*)
and Zone C (active lift_d1 lifter), leaving cold Unity game code and unused
non-trust lean_* fallbacks in the cold tail of .text.

Usage:
    python3 tools/linux_hot_order.py \
        --elf build/linux-aarch64/ps2xRuntime/ps2EntryRunner \
        -o game/overrides/linux_hot_order.ld
"""

import argparse
import bisect
import os
import re
import subprocess
from collections import defaultdict


SKIP_PREFIXES = (
    "_GLOBAL__sub_I_",
    "_Z41__static_initialization",
    "__tcf_",
    "__cxx_global_var_init",
)

HOT_RUNTIME_PATTERNS = (
    "sotc_native",
    "native_renderer",
    "Renderer",
    "GsRenderer",
    "GsFrontend",
    "SoundMixer",
    "PS2Memory",
    "EEScheduler",
    "Ps2Thread",
    "decomp_",
    "pose82",
    "vu1rec_",
)


def is_skip_symbol(sym: str) -> bool:
    return sym.startswith(SKIP_PREFIXES)


def normalize_mangled(sym: str) -> str:
    """Strip GCC IPA clone suffixes and normalize Vita ARM32 -> ARM64 differences."""
    base = re.sub(r"\.(constprop|isra|part|cold|lto_priv)(\.[0-9]+)*$", "", sym)
    base = base.replace("19__simd128_float32_t", "11__Float32x4_t")
    return base


def normalize_vita_uint64(sym: str) -> str:
    """Map ILP32 uint64_t mangling ('y') to LP64 ('m') in parameter lists."""
    out = sym.replace("19__simd128_float32_t", "11__Float32x4_t")
    m = re.match(r"^(_Z(?:N|Z|K)?.*E)(.+)$", out)
    if m:
        head, tail = m.group(1), m.group(2)
        tail = re.sub(r"y(?=[vhcabhstijlmxyefdg]|P|R|K|S|$)", "m", tail)
        out = head + tail
    return out


def is_pure_cold_sub(sym: str) -> bool:
    """True for bare recompiled sub_XXXXXXXX / named PS2 funcs without native override."""
    if any(p in sym for p in HOT_RUNTIME_PATTERNS):
        return False
    return "leant_" not in sym and "lift_d" not in sym


def read_elf_text_symbols(nm_exe: str, elf_path: str):
    out = subprocess.check_output(
        [nm_exe, "--defined-only", "-S", elf_path],
        text=True,
        errors="ignore",
    )
    sym_sizes = {}
    base_to_syms = defaultdict(list)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[2] in ("T", "t", "W", "w"):
            sz = int(parts[1], 16)
            sym = parts[3]
        elif len(parts) == 3 and parts[1] in ("T", "t", "W", "w"):
            sz = 0
            sym = parts[2]
        else:
            continue
        if is_skip_symbol(sym):
            continue
        if sym not in sym_sizes:
            sym_sizes[sym] = sz
            base_to_syms[normalize_mangled(sym)].append(sym)
    return sym_sizes, base_to_syms


def parse_vita_hot_order(path: str):
    syms = []
    if not os.path.exists(path):
        return syms
    pat = re.compile(r"\*\(\.text\.([^\s\)]+)")
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            m = pat.search(line)
            if m:
                syms.append(m.group(1))
    return syms


def parse_lean_hot(path: str):
    addrs = []
    if not os.path.exists(path):
        return addrs
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            parts = s.split()
            try:
                addrs.append(int(parts[0], 16))
            except ValueError:
                continue
    return addrs


def parse_guest_prof(path: str):
    if not path or not os.path.exists(path):
        return []
    hits = defaultdict(int)
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            parts = line.split()
            if len(parts) != 3:
                continue
            try:
                pc = int(parts[1], 16)
                cnt = int(parts[2])
            except ValueError:
                continue
            hits[pc] += cnt
    return [(pc, cnt) for pc, cnt in sorted(hits.items(), key=lambda kv: -kv[1])]


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate linux_hot_order.ld for R36S/aarch64 GNU ld.")
    ap.add_argument("--elf", required=True, help="Path to built ps2EntryRunner ELF")
    ap.add_argument("--nm", default="aarch64-none-linux-gnu-nm", help="nm executable")
    ap.add_argument("--vita-order", default="game/overrides/vita_hot_order.ld", help="Reference vita_hot_order.ld")
    ap.add_argument("--lean-hot", default="game/lean_hot.txt", help="game/lean_hot.txt cycle order")
    ap.add_argument("--guest-prof", default="", help="Optional guest_profile sample file")
    ap.add_argument("--zone-a-leant", type=int, default=400, help="Top leant_* entries promoted to Zone A")
    ap.add_argument("-o", "--output", default="game/overrides/linux_hot_order.ld", help="Output .ld path")
    args = ap.parse_args()

    sym_sizes, base_to_syms = read_elf_text_symbols(args.nm, args.elf)
    print(f"Loaded {len(sym_sizes)} text symbols from {args.elf}")

    addr_re = re.compile(r"_0x([0-9a-fA-F]+)(?:E|P|$)")
    leant_by_addr = defaultdict(list)
    decomp_by_addr = defaultdict(list)

    for sym in sym_sizes:
        m = addr_re.search(sym)
        if not m:
            continue
        addr = int(m.group(1), 16)
        if "leant_" in sym:
            leant_by_addr[addr].append(sym)
        elif "decomp_" in sym:
            decomp_by_addr[addr].append(sym)

    known_addrs = sorted(set(leant_by_addr.keys()) | set(decomp_by_addr.keys()))

    ordered = []
    seen = set()

    def emit(sym: str):
        if sym in sym_sizes and sym not in seen:
            seen.add(sym)
            ordered.append(sym)

    def emit_with_variants(candidate: str):
        emit(candidate)
        norm = normalize_mangled(candidate)
        for s in base_to_syms.get(norm, ()):
            emit(s)
        vnorm = normalize_vita_uint64(candidate)
        if vnorm != norm:
            for s in base_to_syms.get(vnorm, ()):
                emit(s)

    vita_syms = parse_vita_hot_order(args.vita_order)
    lean_hot_addrs = parse_lean_hot(args.lean_hot)

    # Zone A1: Hot runtime & native overrides from vita_hot_order.ld (excluding lift_d2..5 and pure cold subs)
    z_a1_start = len(ordered)
    vita_cold_subs = []
    for vsym in vita_syms:
        if "lift_d" in vsym and not vsym.startswith("_ZN7lift_d1"):
            continue
        if is_pure_cold_sub(vsym):
            vita_cold_subs.append(vsym)
            continue
        emit_with_variants(vsym)
    z_a1_bytes = sum(sym_sizes[s] for s in ordered[z_a1_start:])
    z_a1_cnt = len(ordered) - z_a1_start

    # Zone A2: Top trusted lean functions (game/lean_hot.txt top N, plus top guest_prof hits)
    z_a2_start = len(ordered)
    if known_addrs:
        for pc, _ in parse_guest_prof(args.guest_prof)[:800]:
            idx = bisect.bisect_right(known_addrs, pc) - 1
            if idx >= 0 and (pc - known_addrs[idx]) < 0x10000:
                fn_addr = known_addrs[idx]
                for s in decomp_by_addr.get(fn_addr, ()):
                    emit(s)
                for s in leant_by_addr.get(fn_addr, ()):
                    emit(s)
    for addr in lean_hot_addrs[:args.zone_a_leant]:
        for s in leant_by_addr.get(addr, ()):
            emit(s)
    z_a2_bytes = sum(sym_sizes[s] for s in ordered[z_a2_start:])
    z_a2_cnt = len(ordered) - z_a2_start

    # Zone B1: Any remaining native override & runtime symbols (ensures all .constprop/new decomp stay warm)
    z_b1_start = len(ordered)
    hot_rest = [
        sym for sym in sym_sizes
        if sym not in seen
        and any(p in sym for p in HOT_RUNTIME_PATTERNS)
        and "lean_" not in sym
        and "leant_" not in sym
    ]
    for sym in sorted(hot_rest):
        emit(sym)
    z_b1_bytes = sum(sym_sizes[s] for s in ordered[z_b1_start:])
    z_b1_cnt = len(ordered) - z_b1_start

    # Zone B2: Remaining leant_* in lean_hot order + warm sub_* from vita_hot_order
    z_b2_start = len(ordered)
    for addr in lean_hot_addrs[args.zone_a_leant:]:
        for s in leant_by_addr.get(addr, ()):
            emit(s)
    for sym in sorted(sym_sizes):
        if sym not in seen and "leant_" in sym:
            emit(sym)
    for vsym in vita_cold_subs:
        emit_with_variants(vsym)
    z_b2_bytes = sum(sym_sizes[s] for s in ordered[z_b2_start:])
    z_b2_cnt = len(ordered) - z_b2_start

    # Zone C: Active lift_d1 functions
    z_c_start = len(ordered)
    for sym in sorted(sym_sizes):
        if sym not in seen and sym.startswith("_ZN7lift_d1"):
            emit(sym)
    z_c_bytes = sum(sym_sizes[s] for s in ordered[z_c_start:])
    z_c_cnt = len(ordered) - z_c_start

    mb = 1024 * 1024
    print(
        f"Zone A (critical): {z_a1_cnt} runtime/native ({z_a1_bytes/mb:.2f} MB) + "
        f"{z_a2_cnt} top leant ({z_a2_bytes/mb:.2f} MB) = {(z_a1_bytes+z_a2_bytes)/mb:.2f} MB\n"
        f"Zone B (warm):     {z_b1_cnt} rest native ({z_b1_bytes/mb:.2f} MB) + "
        f"{z_b2_cnt} rest leant+sub ({z_b2_bytes/mb:.2f} MB) = {(z_b1_bytes+z_b2_bytes)/mb:.2f} MB\n"
        f"Zone C (lift_d1):  {z_c_cnt} ({z_c_bytes/mb:.2f} MB)\n"
        f"Total ordered:     {len(ordered)} syms ({(z_a1_bytes+z_a2_bytes+z_b1_bytes+z_b2_bytes+z_c_bytes)/mb:.2f} MB)"
    )

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    with open(args.output, "w", encoding="utf-8", newline="\n") as out:
        out.write(
            "/* Hot code section order for Linux/R36S aarch64 GNU ld --section-ordering-file\n"
            "   (generated by tools/linux_hot_order.py). */\n"
            ".text : {\n"
        )
        for sym in ordered:
            out.write(f"  *(.text.{sym} .text.hot.{sym} .text.startup.{sym})\n")
        out.write("}\n")
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
