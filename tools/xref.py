"""Find code referencing a string or address in the combined ELF (lui/addiu|ori pairs).

Usage: xref.py ELF_FILE FUNCTIONS_CSV QUERY

The ELF file is the combined executable to scan, the functions CSV provides the
function boundaries used to name each reference, and the query is either a
string to look up or an address with a 0x prefix. Run with --help for the full
argument list.
"""
import argparse
import sys
import struct
import bisect
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from elfmini import Elf


def main():
    parser = argparse.ArgumentParser(
        description="Find code referencing a string or address in the combined ELF (lui/addiu|ori pairs)."
    )
    parser.add_argument(
        "elf_file",
        type=Path,
        help="Path to the ELF file (e.g. SOTC.elf)",
    )
    parser.add_argument(
        "functions_csv",
        type=Path,
        help="Path to the functions CSV file (e.g. functions.csv)",
    )
    parser.add_argument(
        "query",
        help="String to search or address with 0x prefix (e.g. 'hello' or '0x80012345')",
    )
    args = parser.parse_args()

    # Existence check for ELF input and functions CSV
    if not args.elf_file.is_file():
        print(f"Error: ELF file not found: {args.elf_file}", file=sys.stderr)
        sys.exit(1)
    if not args.functions_csv.is_file():
        print(f"Error: functions CSV not found: {args.functions_csv}", file=sys.stderr)
        sys.exit(1)

    elf = Elf(str(args.elf_file))

    rows = [l.strip().split(",") for l in open(args.functions_csv, encoding="utf-8").readlines()[1:]]
    starts = [int(r[1], 16) for r in rows]
    q = args.query
    targets = set()
    if q.startswith("0x"):
        targets.add(int(q, 16))
    else:
        needle = q.encode()
        for s in elf.sections:
            if s["type"] == 1:
                blob = elf.data[s["offset"]:s["offset"] + s["size"]]
                i = blob.find(needle)
                while i >= 0:
                    # back up to the start of the C string
                    j = i
                    while j > 0 and blob[j - 1] != 0:
                        j -= 1
                    targets.add(s["addr"] + j)
                    end = blob.find(bytes(1), i)
                    print(f"string at {s['addr'] + j:#x}: {blob[j:end]!r}")
                    i = blob.find(needle, i + 1)
    for lo, hi in elf.exec_ranges():
        for a in range(lo, hi, 4):
            w = elf.read32(a)
            if w >> 26 != 0x0F:
                continue
            rt, up = (w >> 16) & 31, (w & 0xFFFF) << 16
            for k in range(1, 12):
                w2 = elf.read32(a + 4 * k)
                if w2 is None:
                    break
                op = w2 >> 26
                if ((w2 >> 21) & 31) == rt and op in (9, 13, 0x23, 0x2B, 0x21, 0x25, 0x20, 0x24):
                    imm = w2 & 0xFFFF
                    val = (up | imm) if op == 13 else (up + struct.unpack("<h", struct.pack("<H", imm))[0]) & 0xFFFFFFFF
                    if val in targets:
                        i = bisect.bisect_right(starts, a) - 1
                        print(f"  ref at {a:#x} in {rows[i][0]}+{a - starts[i]:#x}")
                    break


if __name__ == "__main__":
    main()
