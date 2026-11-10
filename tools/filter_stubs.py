"""Drop stub bindings whose runtime handler is only a TODO placeholder,
so the original SDK code gets recompiled instead."""
import argparse
import pathlib
import re
import sys

repo_root = pathlib.Path(__file__).resolve().parent.parent

parser = argparse.ArgumentParser(
    description="Drop stub bindings whose runtime handler is only a TODO placeholder."
)
parser.add_argument("runtime_dir", help="runtime directory containing src/lib/**/*.cpp stub sources")
parser.add_argument("config_toml", help="TOML config whose stubs block is filtered in place")
args = parser.parse_args()

runtime = pathlib.Path(args.runtime_dir)
if not runtime.is_absolute():
    runtime = repo_root / runtime

try:
    matched = list(runtime.glob("src/lib/**/*.cpp"))
except OSError as exc:
    print(f"error: cannot read runtime stub sources under {runtime}: {exc}", file=sys.stderr)
    sys.exit(1)
if not matched:
    print(
        f"error: no src/lib/**/*.cpp stub sources matched under {runtime / 'src/lib'}",
        file=sys.stderr,
    )
    sys.exit(1)

todo = set()
for f in matched:
    todo |= set(re.findall(r'TODO_NAMED\("([A-Za-z0-9_]+)"', f.read_text(encoding="utf-8", errors="ignore")))

cfg = pathlib.Path(args.config_toml)
text = cfg.read_text(encoding="utf-8")
m = re.search(r'^stubs = \[(.*?)^\]', text, re.S | re.M)
entries = re.findall(r'"([^"]+)"', m.group(1))
keep, drop = [], []
for e in entries:
    name = e.split("@")[0]
    (drop if name in todo or name.lstrip("_") in todo else keep).append(e)
new = "stubs = [\n" + "".join(f'  "{e}",\n' for e in keep) + "]"
cfg.write_text(text[:m.start()] + new + text[m.end():], encoding="utf-8")
print(f"kept {len(keep)}, dropped {len(drop)}: {[d.split('@')[0] for d in drop]}")
