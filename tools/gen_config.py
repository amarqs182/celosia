"""Generate the PS2Recomp config for the combined ELF.

Stubs = boot ELF stubs from the analyzer config + every named function in the
map whose name has a real (non-TODO) runtime handler.

Usage: python tools/gen_config.py <out_dir> <boot_cfg>
Reads <out_dir>/functions.csv (the function map: a header line, then one
"name,hex_address[,...]" entry per line), <out_dir>/SOTC.elf and the optional
<out_dir>/merged_entries.txt. Writes <out_dir>/config.toml.
"""
import argparse, re, sys, pathlib

root = pathlib.Path(__file__).resolve().parent.parent
runtime = root / "PS2Recomp/ps2xRuntime"


def load_function_map(path):
    """Parse the function map CSV into (name, address) rows.

    Expected shape: a header line, then one "name,hex_address[,...]" entry
    per line. Exits with a clear error naming the file and the expected
    shape if the file is missing or malformed.
    """
    if not path.exists():
        sys.exit(f"gen_config: function map not found: {path} "
                 f"(expected CSV with a header line and 'name,hex_address[,...]' rows)")
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        sys.exit(f"gen_config: malformed function map {path}: not valid UTF-8 "
                 f"({exc.reason}) (expected CSV with a header line and 'name,hex_address[,...]' rows)")
    rows = []
    for lineno, raw in enumerate(text.splitlines()[1:], start=2):
        if not raw.strip():
            sys.exit(f"gen_config: malformed function map {path} line {lineno}: empty line "
                     f"(expected 'name,hex_address[,...]')")
        parts = raw.split(",")
        if len(parts) < 2:
            sys.exit(f"gen_config: malformed function map {path} line {lineno}: {raw!r} "
                     f"(expected 'name,hex_address[,...]')")
        name, start = parts[0], parts[1]
        try:
            addr = int(start, 16)
        except ValueError:
            sys.exit(f"gen_config: malformed function map {path} line {lineno}: "
                     f"bad hex address {start!r} (expected 'name,hex_address[,...]')")
        rows.append((name, addr))
    return rows


parser = argparse.ArgumentParser(description="Generate the PS2Recomp config TOML from the function map.")
parser.add_argument("out_dir", help="output directory containing functions.csv and SOTC.elf")
parser.add_argument("boot_cfg", help="analyzer boot TOML to read boot stubs from")
args = parser.parse_args()

calls = (runtime / "include/ps2_call_list.h").read_text(encoding="utf-8")
handlers = set(re.findall(r"X\((\w+)\)", calls))
todo = set()
for f in runtime.glob("src/lib/**/*.cpp"):
    todo |= set(re.findall(r'TODO_NAMED\("(\w+)"', f.read_text(encoding="utf-8", errors="ignore")))
usable = handlers - todo

out_dir = pathlib.Path(args.out_dir)
boot_cfg_path = pathlib.Path(args.boot_cfg)
if not boot_cfg_path.is_file():
    sys.exit(f"gen_config: boot config not found: {boot_cfg_path}")
boot_cfg = boot_cfg_path.read_text(encoding="utf-8")
boot_match = re.search(r"^stubs = \[(.*?)^\]", boot_cfg, re.S | re.M)
if not boot_match:
    sys.exit(f"gen_config: no 'stubs = [...]' block found in {boot_cfg_path}")
boot_stubs = re.findall(r'"([^"]+@0x[0-9A-Fa-f]+)"', boot_match.group(1))
deny = set(l.strip() for l in open(root / "tools/stub_deny.txt", encoding="utf-8")
            if l.strip() and not l.startswith("#")) \
    if (root / "tools/stub_deny.txt").exists() else set()

stubs = {int(s.split("@")[1], 16): s for s in boot_stubs if s.split("@")[0] not in deny}
func_map = load_function_map(out_dir / "functions.csv")
for name, a in func_map:
    if name in usable and name not in deny and a not in stubs:
        stubs[a] = f"{name}@0x{a:08X}"

extra = root / "game/extra_stubs.txt"
if extra.exists():
    for l in extra.read_text(encoding="utf-8").splitlines():
        l = l.strip()
        if l and not l.startswith("#"):
            stubs[int(l.split("@")[1], 16)] = l

# Leaves packed inside a mapped function: entered through the owner's switch.
sys.path.insert(0, str(root / "tools"))
from elfmini import Elf
import funcmap_elf
elf_path = out_dir / "SOTC.elf"
if not elf_path.is_file():
    sys.exit(f"gen_config: combined ELF not found: {elf_path}")
elf = Elf(str(elf_path))
words = {a: elf.read32(a) for lo, hi in elf.exec_ranges() for a in range(lo, hi, 4)}
starts = {a for _, a in func_map}
entries = set(funcmap_elf.hidden_entries(words, starts))
# Starts tools/build_elf.py merged into the function around them (false
# splits): still entered from outside (pointers, jump tables, calls).
merged = out_dir / "merged_entries.txt"
if merged.exists():
    entries |= {int(l, 16) for l in merged.read_text(encoding="utf-8").splitlines() if l.strip() and not l.startswith("#")}
entries = sorted(t for t in entries if t not in stubs and t not in starts)
entry_lines = "".join(f'  "entry_{t:08X}@0x{t:08X}",\n' for t in entries)

stub_lines = "".join(f'  "{stubs[a]}",\n' for a in sorted(stubs))
(out_dir / "config.toml").write_text(f'''# Generated by tools/gen_config.py
[general]
input = "SOTC.elf"
ghidra_output = "functions.csv"
dynamic_immediates = "dynamic_imm.txt"
output = "./output/"
single_file_output = false
patch_syscalls = false
patch_cop0 = true
patch_cache = true
# Guest registers in host locals (ps2xRecomp register_cache.cpp): since
# round 18 within basic blocks, stored at the write (write-through), with
# round 16's calls; 1-2% in calibrated Vita3K (docs/decomp.md, round 18).
# false: round 17's generated code byte for byte.
register_cache = true
stubs = [
{stub_lines}]
skip = []
entry_points = [
{entry_lines}]
''')
print(f"{len(stubs)} stubs, {len(entries)} hidden entry points")
