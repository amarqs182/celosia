#!/usr/bin/env python3
"""Offline PC ASTC texture cache packer for Celosia R36S (Mali-G31).

Converts raw level-0 RGBA dumps produced by SOTC_MALI_DUMP_TEX=1
(`<id_hex>_<W>x<H>.rgba`) into hardware-compressed `<id_hex>.astc` files
ready to copy to `/roms/ports/.SotC_Celosia/tex_cache/` on the R36S.

Zero external Python dependencies (uses pure-Python PNG writer + `astcenc`).
Usage:
    python3 tools/astc_pack_cache.py --in /tmp/celosia_tex_dump --out ./tex_cache --astcenc astcenc
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

FNAME_RE = re.compile(r"^([0-9a-fA-F]{16})_(\d+)x(\d+)\.rgba$")


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    stride = width * 4
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        start = y * stride
        raw.extend(rgba[start : start + stride])

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), level=1))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def choose_block(width: int, height: int, block_small: str, block_large: str) -> str:
    return block_large if (width >= 128 and height >= 128) else block_small


def main() -> int:
    ap = argparse.ArgumentParser(description="Pack Celosia raw RGBA texture dumps into Mali-G31 ASTC cache files.")
    ap.add_argument("--in", dest="in_dir", required=True, help="Input directory containing <id_hex>_<W>x<H>.rgba dumps")
    ap.add_argument("--out", dest="out_dir", required=True, help="Output tex_cache directory for <id_hex>.astc files")
    ap.add_argument("--astcenc", default="astcenc", help="Path to astcenc binary (from ARM astc-encoder)")
    ap.add_argument("--block-small", default="4x4", help="ASTC block size for textures < 128x128 (default: 4x4)")
    ap.add_argument("--block-large", default="6x6", help="ASTC block size for textures >= 128x128 (default: 6x6)")
    ap.add_argument("--quality", default="-medium", help="astcenc quality preset (default: -medium)")
    ap.add_argument("--force", action="store_true", help="Re-encode existing .astc files")
    args = ap.parse_args()

    in_dir = Path(args.in_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    files = sorted(in_dir.glob("*.rgba"))
    if not files:
        print(f"No .rgba texture dumps found in {in_dir}", file=sys.stderr)
        return 1

    ok = 0
    skipped = 0
    total_rgba_bytes = 0
    total_astc_bytes = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp_png = Path(tmp) / "in.png"
        for f in files:
            m = FNAME_RE.match(f.name)
            if not m:
                skipped += 1
                continue
            tex_id, w_s, h_s = m.group(1).lower(), m.group(2), m.group(3)
            w, h = int(w_s), int(h_s)
            rgba = f.read_bytes()
            if len(rgba) != w * h * 4:
                print(f"[skip] {f.name}: expected {w*h*4} bytes, got {len(rgba)}", file=sys.stderr)
                skipped += 1
                continue

            out_astc = out_dir / f"{tex_id}.astc"
            if out_astc.exists() and not args.force:
                skipped += 1
                continue
            tmp_astc = out_dir / f"{tex_id}.astc.tmp"

            write_png(tmp_png, w, h, rgba)
            block = choose_block(w, h, args.block_small, args.block_large)
            cmd = [args.astcenc, "-cl", str(tmp_png), str(tmp_astc), block, args.quality, "-silent"]
            res = subprocess.run(cmd, capture_output=True, text=True)
            if res.returncode != 0:
                tmp_astc.unlink(missing_ok=True)
                print(f"[err] {f.name}: {res.stderr.strip()}", file=sys.stderr)
                skipped += 1
                continue
            os.replace(tmp_astc, out_astc)
            total_rgba_bytes += len(rgba)
            total_astc_bytes += out_astc.stat().st_size
            ok += 1

    print(f"Packed {ok} ASTC textures into {out_dir} (skipped {skipped}).")
    if total_astc_bytes > 0:
        ratio = total_rgba_bytes / total_astc_bytes
        pct = (total_astc_bytes / total_rgba_bytes) * 100.0
        print(
            f"Size: rgba={total_rgba_bytes} bytes ({total_rgba_bytes / (1024 * 1024):.2f} MiB) "
            f"-> astc={total_astc_bytes} bytes ({total_astc_bytes / (1024 * 1024):.2f} MiB) "
            f"({ratio:.2f}x, {pct:.1f}%)"
        )
    else:
        print(f"Size: rgba={total_rgba_bytes} bytes -> astc={total_astc_bytes} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
