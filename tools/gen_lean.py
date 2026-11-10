#!/usr/bin/env python3
"""Lean copies of the hot recompiled functions (game/overrides/lean_hot.cpp).

The hot set is game/lean_hot.txt (guest addresses, hottest first: made from
SOTC_CALL_COUNT_DUMP files with --from). This writes, under build/runner/lean/
(next to the recompiled code; ./celosia recompile wipes it, run this again):
  decls.inc     the lean functions' declarations
  renames.inc   #define <name> lean_<name> for each (direct calls between
                lean functions stay lean)
  decls_trust.inc, renames_trust.inc  the same for the second set
                (leant_<name>, game/overrides/lean_part_tN.cpp: compiled with
                PS2X_EE_TRUST_FINITE, SOTC_EE_TRUST picks it)
  table.inc     LEAN_ENTRY(address, lean_<name>, <name>, "<name>", checkable)
                lines (checkable: no syscall inside, for SOTC_LEAN_CHECK)
  partN.inc     the #includes of the recompiled sources, split over the
                game/overrides/lean_partN.cpp files (compiled in parallel)

After a run, rebuild the lean parts with CCACHE_RECACHE=1 (touch
game/overrides/lean_part*.cpp first): ccache's direct mode does not see a
__has_include that turned true and hands back the old, empty objects (a
link error "undefined reference to lean_...").

Usage:
  tools/gen_lean.py                       # from game/lean_hot.txt
  tools/gen_lean.py --from calls.txt [calls2.txt ...] [--min 1000]
                                          # rewrites game/lean_hot.txt first
  tools/gen_lean.py --help                # this message
"""
import argparse
import glob
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
RUNNER = ROOT / 'build' / 'runner'
LIST = ROOT / 'game' / 'lean_hot.txt'
OUT = RUNNER / 'lean'
PARTS = 12  # game/overrides/lean_part0.cpp .. lean_part11.cpp

PARSER = argparse.ArgumentParser(
    prog='gen_lean.py',
    description='Write lean copies of the hot recompiled functions (build/runner/lean).',
    epilog=('usage:\n'
            '  tools/gen_lean.py\n'
            '  tools/gen_lean.py --from calls.txt [calls2.txt ...] [--min 1000]\n'),
    formatter_class=argparse.RawDescriptionHelpFormatter)
PARSER.add_argument('--from', dest='from_', nargs='*', metavar='CALLS',
                     help='rewrite game/lean_hot.txt from these call-count dumps')
PARSER.add_argument('--min', type=int, default=1000, metavar='N',
                     help='minimum number of calls kept (default: 1000)')


def open_text(path, mode='r'):
    """Open a text file with UTF-8; exit with a clear error if it is missing."""
    try:
        return open(path, mode, encoding='utf-8')
    except FileNotFoundError:
        sys.exit(f'gen_lean.py: missing file: {path}')


def runner_functions():
    by_address = {}
    for path in glob.glob(os.path.join(RUNNER, '*_0x*.cpp')):
        name = os.path.basename(path)[:-4]
        m = re.match(r'.*_0x([0-9a-f]+)$', name)
        if m:
            by_address[int(m.group(1), 16)] = name
    return by_address


def write_list(dumps, minimum):
    counts = {}
    for path in dumps:
        for line in open_text(path):
            if line.startswith('#'):
                continue
            a, c = line.split()
            counts[int(a, 16)] = max(counts.get(int(a, 16), 0), int(c))
    hot = sorted((a for a, c in counts.items() if c >= minimum), key=lambda a: -counts[a])
    with open_text(LIST, 'w') as out:
        out.write('# Hot recompiled functions for the lean copies (tools/gen_lean.py), hottest first.\n')
        out.write('# From %s, at least %d calls.\n' % (', '.join(os.path.basename(d) for d in dumps), minimum))
        for a in hot:
            out.write('%08x %d\n' % (a, counts[a]))
    print('%s: %d functions' % (LIST, len(hot)))


def main():
    args, _unknown = PARSER.parse_known_args()
    if args.from_ is not None:
        write_list(args.from_, args.min)
    funcs = runner_functions()
    names = []
    for line in open_text(LIST):
        if line.startswith('#') or not line.strip():
            continue
        a = int(line.split()[0], 16)
        if a not in funcs:  # not in build/runner: nothing to copy
            continue
        with open_text(os.path.join(RUNNER, funcs[a] + '.cpp')) as f:
            src = f.read()
        if '// Function:' not in src:  # a stub into the host kernel (WaitSema ...): no guest code
            continue
        # A syscall inside: left out of the difftest (it can switch threads).
        names.append((a, funcs[a], 'handleSyscall' in src))
    os.makedirs(OUT, exist_ok=True)
    for f in glob.glob(os.path.join(OUT, '*.inc')):
        os.remove(f)
    sig = '(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)'
    with open_text(os.path.join(OUT, 'decls.inc'), 'w') as out:
        for _, n, _ in names:
            out.write('void lean_%s%s;\n' % (n, sig))
    with open_text(os.path.join(OUT, 'renames.inc'), 'w') as out:
        for _, n, _ in names:
            out.write('#define %s lean_%s\n' % (n, n))
    # The second set (round 26, lean_part_tN.cpp, PS2X_EE_TRUST_FINITE): leant_<name>.
    with open_text(os.path.join(OUT, 'decls_trust.inc'), 'w') as out:
        for _, n, _ in names:
            out.write('void leant_%s%s;\n' % (n, sig))
    with open_text(os.path.join(OUT, 'renames_trust.inc'), 'w') as out:
        for _, n, _ in names:
            out.write('#define %s leant_%s\n' % (n, n))
    with open_text(os.path.join(OUT, 'table.inc'), 'w') as out:
        for a, n, sys_ in names:
            out.write('LEAN_ENTRY(0x%08xu, lean_%s, %s, "%s", %s)\n' % (a, n, n, n, 'false' if sys_ else 'true'))
    # Spread by size so the parts take about as long to compile.
    sizes = sorted(((os.path.getsize(os.path.join(RUNNER, n + '.cpp')), n) for _, n, _ in names), reverse=True)
    parts = [[] for _ in range(PARTS)]
    load = [0] * PARTS
    for size, n in sizes:
        i = load.index(min(load))
        parts[i].append(n)
        load[i] += size
    for i, part in enumerate(parts):
        with open_text(os.path.join(OUT, 'part%d.inc' % i), 'w') as out:
            for n in sorted(part):
                out.write('#include "%s.cpp"\n' % n)
    print('%s: %d lean functions in %d parts (%.1f MB of recompiled C++)' % (OUT, len(names), PARTS, sum(load) / 1e6))


if __name__ == '__main__':
    main()
