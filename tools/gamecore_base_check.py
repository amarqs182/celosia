"""Does a module in a RAM dump sit where the recompiled code expects it?

The recompiled GAMECORE code was lifted from the RAM dump `./celosia setup`
captured (build/captures/gamecore.bin), so its addresses are absolute. If the
game's loader puts the module somewhere else at run time (boot timing has moved
it by 0x40 and by 0x600 before: docs/decomp.md, SOTC_LOG_MALLOC), the natives
and the recompiled functions run on other code and the boot stalls after the
loader's JALR.

Usage: gamecore_base_check.py CAPTURE.bin DEVICE.bin [--base 0x1344780]
                              [--words 256] [--span 0x2000]

CAPTURE.bin is the setup's dump, DEVICE.bin a dump of the run being checked
(PS2X_DUMP_RAM_PATH, taken after GAMECORE is loaded). Both are 32 MB of EE RAM.
A window of --words instructions at --base in the capture is searched in the
device dump within +-span bytes. j/jal compare by opcode only (their targets
move with the module) and lui is skipped; every other word must be equal. The
jal targets' difference at the best offset is the relocation shift itself.
"""
import argparse
import collections
import struct
import sys

RAM_SIZE = 32 * 1024 * 1024
J, JAL, LUI = 2, 3, 0x0F


def load(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        sys.exit(f"{path}: {e.strerror}")
    if len(data) < RAM_SIZE:
        sys.exit(f"{path}: {len(data)} bytes, expected a {RAM_SIZE}-byte EE RAM dump")
    return data


def window(data, address, count):
    if address < 0 or address + 4 * count > RAM_SIZE:
        return None
    return struct.unpack_from(f"<{count}I", data, address)


def compare(ref, dev):
    """(score, jal target differences in bytes) of dev against ref."""
    scored = equal = 0
    diffs = collections.Counter()
    for r, d in zip(ref, dev):
        op = r >> 26
        if op == LUI:
            continue
        scored += 1
        if op in (J, JAL):
            if d >> 26 == op:
                equal += 1
                diffs[((d & 0x3FFFFFF) - (r & 0x3FFFFFF)) * 4] += 1
        elif r == d:
            equal += 1
    return (equal / scored if scored else 0.0), diffs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("capture")
    ap.add_argument("device")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x1344780)
    ap.add_argument("--words", type=int, default=256)
    ap.add_argument("--span", type=lambda s: int(s, 0), default=0x2000)
    args = ap.parse_args()

    ref = window(load(args.capture), args.base, args.words)
    if ref is None:
        sys.exit("--base/--words outside RAM")
    device = load(args.device)

    results = []
    for delta in range(-args.span, args.span + 1, 4):
        dev = window(device, args.base + delta, args.words)
        if dev is not None:
            score, diffs = compare(ref, dev)
            results.append((score, -abs(delta), delta, diffs))
    results.sort(reverse=True)

    print(f"window: {args.words} words at {args.base:#x} in the capture")
    for score, _, delta, _ in results[:3]:
        print(f"  device offset {delta:+#x}: {score:.1%} of the compared words match")
    score, _, delta, diffs = results[0]
    if diffs and score >= 0.5:
        shift, count = diffs.most_common(1)[0]
        print(f"  jal targets at the best offset move by {shift:+#x} "
              f"({count} of {sum(diffs.values())})")

    if score < 0.5:
        print("verdict: no alignment within the span matches - a different module "
              "or build, or the module is not loaded yet in this dump")
    elif delta == 0:
        print("verdict: same placement as the capture")
    else:
        print(f"verdict: the module sits {delta:+#x} bytes from where the recompiled "
              "code expects it")


if __name__ == "__main__":
    main()
