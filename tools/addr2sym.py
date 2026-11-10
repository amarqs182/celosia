"""Resolve addresses to function+offset using a recomp functions.csv.

Inputs:
    functions.csv (positional): CSV with columns name,start,end (hex). First line is header.
    address (positional, repeatable): Hex address(es) to resolve.

Output:
    For each address: "<addr> = <function>+<offset>" if inside a function range,
    otherwise "<addr> = ?".
"""

import argparse
import bisect
import sys


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Resolve addresses to function+offset using a recomp functions.csv.",
        add_help=True,
    )
    parser.add_argument(
        "csv_path",
        help="Path to functions.csv (columns: name,start,end; hex addresses; first line header).",
    )
    parser.add_argument(
        "addresses",
        nargs="+",
        help="Hex address(es) to resolve.",
    )
    return parser.parse_args()


def load_functions(csv_path: str):
    try:
        with open(csv_path, encoding="utf-8") as f:
            lines = f.readlines()
    except FileNotFoundError:
        sys.exit(f"error: functions.csv not found: {csv_path}")
    except OSError as e:
        sys.exit(f"error: cannot read {csv_path}: {e}")

    if len(lines) < 2:
        sys.exit(f"error: {csv_path}: no data rows (expected header + at least one row)")

    rows = [l.strip().split(",") for l in lines[1:]]
    for i, r in enumerate(rows):
        if len(r) != 3:
            sys.exit(f"error: {csv_path}: line {i + 2}: expected 3 columns, got {len(r)}")
    return rows


def parse_address(addr: str, csv_path: str) -> int:
    try:
        return int(addr, 16)
    except ValueError:
        sys.exit(
            f"error: malformed address: {addr} (in {csv_path}; "
            "expected hex, e.g. 0x1234 or 1234)"
        )


def main() -> None:
    args = parse_args()
    rows = load_functions(args.csv_path)
    starts = [int(r[1], 16) for r in rows]

    for addr_str in args.addresses:
        v = parse_address(addr_str, args.csv_path)
        i = bisect.bisect_right(starts, v) - 1
        if i >= 0 and v < int(rows[i][2], 16):
            print(f"{v:#x} = {rows[i][0]}+{v - starts[i]:#x}")
        else:
            print(f"{v:#x} = ?")


if __name__ == "__main__":
    main()
