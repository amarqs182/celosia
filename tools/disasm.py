"""Disassemble a function (or address range) of the combined game ELF.

ELF in: the combined game ELF produced by the recompile step (the
``recomp/game/SOTC.elf`` file under the build tree, which defaults to
``build`` and can be overridden with the ``CELOSIA_BUILD`` environment
variable), read together with its ``functions.csv`` and ``symbols.txt``
companions.  Disassembly out: a capstone MIPS listing on stdout, one
line per instruction (labels and branch-target comments included).

Usage: disasm.py <name|0xstart> [0xend]

Address range arguments: ``target`` is either a function name from
``functions.csv`` or a hex address ``0xstart``; ``end`` is an optional
hex end address ``0xend`` that defaults to ``0xstart + 0x100`` for a raw
hex start (it is ignored when a function name is given).
"""

import argparse
import os
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfmini import Elf

GAME = os.path.join(os.environ.get("CELOSIA_BUILD", "build"), "recomp/game")
ELF_PATH = f"{GAME}/SOTC.elf"
CSV_PATH = f"{GAME}/functions.csv"
SYM_PATH = f"{GAME}/symbols.txt"

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

parser = argparse.ArgumentParser(
    prog="disasm.py",
    description="Disassemble a function (or address range) of the combined game ELF.",
)
parser.add_argument("target", help="Function name or hex address (0xstart)")
parser.add_argument("end", nargs="?", default=None, help="Hex end address (0xend, optional)")
args = parser.parse_args()

for path in (ELF_PATH, CSV_PATH, SYM_PATH):
    if not os.path.isfile(path):
        print(f"Error: file not found: {path}", file=sys.stderr)
        sys.exit(1)

elf = Elf(ELF_PATH)

with open(CSV_PATH, encoding="utf-8") as f:
    rows = [l.strip().split(",") for l in f.readlines()[1:]]

byname = {r[0]: (int(r[1], 16), int(r[2], 16)) for r in rows}
names = {int(r[1], 16): r[0] for r in rows}
starts = [int(r[1], 16) for r in rows]

with open(SYM_PATH, encoding="utf-8") as f:
    for l in f:
        a, n = l.split()[:2]
        names.setdefault(int(a, 16), n)


def parse_hex(text):
    try:
        return int(text, 16)
    except ValueError:
        print(f"Error: invalid hexadecimal address {text!r} (expected like 0x80020000)", file=sys.stderr)
        sys.exit(1)


q = args.target
if q.startswith("0x"):
    lo = parse_hex(q)
    hi = parse_hex(args.end) if args.end else lo + 0x100
else:
    try:
        lo, hi = byname[q]
    except KeyError:
        print(f"Error: unknown function name '{q}'", file=sys.stderr)
        sys.exit(1)

if hi <= lo:
    print(f"Error: invalid address range in {ELF_PATH}: 0x{lo:08x}-0x{hi:08x} (end must be above start)", file=sys.stderr)
    sys.exit(1)

words = [elf.read32(a) for a in range(lo, hi, 4)]
if any(w is None for w in words):
    print(f"Error: address range not mapped in {ELF_PATH}: 0x{lo:08x}-0x{hi:08x}", file=sys.stderr)
    sys.exit(1)
code = b"".join(w.to_bytes(4, "little") for w in words)
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 | capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata = True  # EE-only (MMI) opcodes show as .byte
for ins in md.disasm(code, lo):
    note = ""
    if ins.mnemonic in ("jal", "j"):
        t = int(ins.op_str, 16)
        note = f"  ; {names.get(t, '?')}"
    label = f"{names[ins.address]}:\n" if ins.address in names and ins.address != lo else ""
    print(f"{label}  {ins.address:08x}: {ins.mnemonic:8} {ins.op_str}{note}")
