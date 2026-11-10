"""List static (jal/j) callers of a function, recursively up to a depth.

Input: a function address (0x-prefixed hex) or a function name, plus an
optional recursion depth (default: 3).
Output: one indented line per caller ("<name> (<addr>)"), recursing into
each caller's own static callers up to the given depth; functions without
static callers are annotated accordingly.

Usage: callers.py <name|0xaddr> [depth]"""
import os, sys, bisect, argparse
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from elfmini import Elf
GAME = os.path.join(os.environ.get('CELOSIA_BUILD', 'build'), 'recomp/game')
ELF_PATH = f'{GAME}/SOTC.elf'

parser = argparse.ArgumentParser(description="List static (jal/j) callers of a function, recursively up to a depth.")
parser.add_argument("query", help="function name or 0x-prefixed address")
parser.add_argument("depth", nargs="?", type=int, default=3, help="recursion depth (default: 3)")
args = parser.parse_args()
if not os.path.exists(ELF_PATH):
    sys.exit(f"error: ELF input not found: {ELF_PATH}")
e = Elf(ELF_PATH)
rows = [l.strip().split(",") for l in open(f'{GAME}/functions.csv', encoding='utf-8').readlines()[1:]]
starts = [int(r[1], 16) for r in rows]
byname = {r[0]: int(r[1], 16) for r in rows}
def func_of(a):
    i = bisect.bisect_right(starts, a) - 1
    return starts[i], rows[i][0]
callers = {}
for lo, hi in e.exec_ranges():
    for a in range(lo, hi, 4):
        w = e.read32(a)
        if w >> 26 in (2, 3):
            t = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            callers.setdefault(t, set()).add(func_of(a))
q = args.query
start = int(q, 16) if q.startswith("0x") else byname[q]
depth = args.depth
seen = set()
def show(addr, name, d):
    print("  " * d + f"{name} ({addr:#x})" + ("" if callers.get(addr) else "   <- no static callers (called via pointer?)"))
    if d >= depth or addr in seen:
        return
    seen.add(addr)
    for ca, cn in sorted(callers.get(addr, ())):
        show(ca, cn, d + 1)
show(start, func_of(start)[1], 0)
