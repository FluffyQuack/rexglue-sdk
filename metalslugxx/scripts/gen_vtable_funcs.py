#!/usr/bin/env python3
"""
gen_vtable_funcs.py - structural vtable / code-pointer-table scanner.

Why this exists
---------------
ReXGlue's built-in vtable discovery (src/codegen/vtable_scanner.cpp) is gated on
MSVC RTTI Complete Object Locators (it looks for `.?AV`/`.?AU` type descriptors
in .rdata). Metal Slug XX was compiled with RTTI **disabled** for game code
(only 6 std:: COLs exist in the whole image), so that scanner finds essentially
no vtables. Every function reachable *only* through a vtable slot (adjustor
thunks, no-op `blr` virtuals, ordinary virtual methods) is therefore never
discovered or registered, and the first indirect call to one traps at runtime
with "Call to invalid or unregistered function at guest address 0x...".

This script recovers those entry points structurally, without RTTI:
  * scan .rdata and the file-backed part of .data for 4-byte big-endian values
    that are valid, 4-aligned pointers into the executable range;
  * group them into maximal runs of consecutive code-pointers (a vtable / fnptr
    table is exactly such a run);
  * within runs of length >= MIN_RUN, accept a slot target T only if the
    instruction at T-4 is an *unconditional* function terminator (blr / bctr /
    unconditional b/ba / zero padding) -- i.e. T begins right after a previous
    function ends. That single check makes T a genuine function entry and rules
    out mid-function false positives that would otherwise create overlapping
    functions and break codegen.

Output is a TOML file of size-less `[functions]` entries (codegen auto-discovers
each extent from the entry). Entries already present in metalslugxx_config.toml
are skipped so their explicit size hints are preserved.

Run from the metalslugxx project dir:
    python3 scripts/gen_vtable_funcs.py
"""
import re
import struct
import sys
from pathlib import Path

BASE = 0x82000000
EXLO, EXHI = 0x821F0000, 0x82BCE838  # executable range (.text + .embsec_*)
MIN_RUN = 2                          # minimum consecutive code-pointers to call it a table
# A run is treated as a vtable / fnptr-table (targets are distinct functions)
# only if its targets span at least MIN_SPREAD bytes. Smaller-spread runs are
# jump/switch tables: their "targets" are internal case blocks of ONE function,
# and registering those as functions would split it and break codegen. Observed
# jump tables here span <=0x388; real vtables span >=0x22400 -- a ~100x gap.
MIN_SPREAD = 0x2000

PROJ = Path(__file__).resolve().parent.parent
BASEFILE = PROJ.parent / "Assets" / "58410972" / "default.basefile.exe"
CONFIG = PROJ / "metalslugxx_config.toml"
OUT = PROJ / "metalslugxx_vtables.toml"

# data regions to scan: (name, lo_addr, hi_addr) -- file-backed bytes only
REGIONS = [
    ("rdata", 0x82000600, 0x821B9670),
    ("data",  0x82BD0000, 0x82CF0400),  # raw size 0x120400; beyond is zero-init
]


def main():
    d = BASEFILE.read_bytes()

    def be32(off):
        return struct.unpack_from(">I", d, off)[0]

    def insn(addr):
        return be32(addr - BASE)

    def is_cp(v):
        return EXLO <= v < EXHI and (v & 3) == 0

    def is_terminator(w):
        # unconditional terminators only (so T-4 ending => T is a function start)
        if w == 0x4E800020:      # blr
            return True
        if w == 0x4E800420:      # bctr
            return True
        if w == 0:               # alignment padding
            return True
        if (w >> 26) == 18 and (w & 1) == 0:  # b / ba (LK=0; bl/bla are calls)
            return True
        return False

    # existing config function keys (bare or quoted hex) -- preserve their hints
    existing = set()
    if CONFIG.exists():
        for m in re.finditer(r'"?0x([0-9A-Fa-f]{8})"?\s*=', CONFIG.read_text()):
            existing.add(int(m.group(1), 16))

    targets = {}     # addr -> source vtable address (first seen)
    vtable_count = 0
    slot_total = 0

    for nm, lo, hi in REGIONS:
        o = lo - BASE
        end = hi - BASE
        while o < end:
            if not is_cp(be32(o)):
                o += 4
                continue
            # extend a run of consecutive code-pointers
            run_start = o
            while o < end and is_cp(be32(o)):
                o += 4
            run_len = (o - run_start) // 4
            if run_len < MIN_RUN:
                continue
            run_vals = [be32(run_start + i * 4) for i in range(run_len)]
            if (max(run_vals) - min(run_vals)) < MIN_SPREAD:
                continue  # jump/switch table (targets within one function), not a vtable
            vtable_count += 1
            for i in range(run_len):
                slot_off = run_start + i * 4
                t = be32(slot_off)
                slot_total += 1
                if t <= EXLO:
                    continue
                if not is_terminator(insn(t - 4)):
                    continue
                if t in existing:
                    continue
                targets.setdefault(t, BASE + run_start)

    out = sorted(targets)
    with OUT.open("w", encoding="utf-8") as f:
        f.write("# AUTO-GENERATED by scripts/gen_vtable_funcs.py -- do not hand-edit.\n")
        f.write("#\n")
        f.write("# Vtable / code-pointer-table entry points recovered structurally\n")
        f.write("# (the game has RTTI disabled, so the SDK's RTTI-based vtable scanner\n")
        f.write("# finds nothing). Each is a CONFIG function entry; codegen auto-sizes\n")
        f.write("# it. Entries already in metalslugxx_config.toml are omitted here.\n")
        f.write("#\n")
        f.write(f"# regions scanned : {', '.join(n for n,_,_ in REGIONS)}\n")
        f.write(f"# exec range      : 0x{EXLO:08X}-0x{EXHI:08X}\n")
        f.write(f"# min run length  : {MIN_RUN}\n")
        f.write(f"# min target span : 0x{MIN_SPREAD:X} (smaller = jump table, excluded)\n")
        f.write(f"# tables found    : {vtable_count}  (slots inspected: {slot_total})\n")
        f.write(f"# NEW entries     : {len(out)}  (already in config: skipped)\n")
        f.write("\n[functions]\n")
        for t in out:
            f.write(f'"0x{t:08X}" = {{ }}\n')

    print(f"basefile        : {BASEFILE}")
    print(f"tables found    : {vtable_count} (>= {MIN_RUN} ptrs)")
    print(f"slots inspected : {slot_total}")
    print(f"existing config : {len(existing)} keys (skipped)")
    print(f"NEW entries     : {len(out)}")
    print(f"wrote           : {OUT}")


if __name__ == "__main__":
    sys.exit(main())
