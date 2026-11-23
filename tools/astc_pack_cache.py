#!/usr/bin/env python3
"""Offline PC ASTC texture cache packer for Celosia R36S (Mali-G31).

Converts raw RGBA dumps produced by SOTC_MALI_DUMP_TEX=1:
  - multi-mip:  `<hash_hex>_<W>x<H>_L<levels>.rgba`
  - single-mip: `<hash_hex>_<W>x<H>.rgba`
into multi-level `.astcm` (`MTC1` container) files ready to copy to
`/roms/ports/.SotC_Celosia/tex_cache/` on the R36S, where Mali-G31 uploads
every mip level directly into `GL_TEXTURE_2D` and `GL_TEXTURE_2D_ARRAY`.

Zero external Python dependencies (uses pure-Python PNG writer + `astcenc`).
Usage:
    python3 tools/astc_pack_cache.py --in /tmp/celosia_tex_dump --out ./tex_cache --astcenc astcenc
"""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

FNAME_RE = re.compile(r"^([0-9a-fA-F]{16})_(\d+)x(\d+)(?:_L(\d+))?\.rgba$")


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    stride = width * 4
    raw = bytearray()
    for y in range(height):
        raw.append(0)
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


def downsample_box2x2(rgba: bytes, w: int, h: int) -> tuple[int, int, bytes]:
    nw = max(1, w >> 1)
    nh = max(1, h >> 1)
    out = bytearray(nw * nh * 4)
    src = memoryview(rgba)
    for y in range(nh):
        y0 = min(h - 1, y * 2)
        y1 = min(h - 1, y * 2 + 1)
        row0 = y0 * w * 4
        row1 = y1 * w * 4
        drow = y * nw * 4
        for x in range(nw):
            x0 = min(w - 1, x * 2) * 4
            x1 = min(w - 1, x * 2 + 1) * 4
            p00 = row0 + x0
            p10 = row0 + x1
            p01 = row1 + x0
            p11 = row1 + x1
            dst = drow + x * 4
            out[dst + 0] = (src[p00 + 0] + src[p10 + 0] + src[p01 + 0] + src[p11 + 0] + 2) >> 2
            out[dst + 1] = (src[p00 + 1] + src[p10 + 1] + src[p01 + 1] + src[p11 + 1] + 2) >> 2
            out[dst + 2] = (src[p00 + 2] + src[p10 + 2] + src[p01 + 2] + src[p11 + 2] + 2) >> 2
            out[dst + 3] = (src[p00 + 3] + src[p10 + 3] + src[p01 + 3] + src[p11 + 3] + 2) >> 2
    return nw, nh, bytes(out)


def choose_block(width: int, height: int, block_small: str, block_large: str) -> tuple[int, int, str]:
    blk = block_large if (width >= 128 and height >= 128) else block_small
    bx_s, by_s = blk.lower().split("x", 1)
    return int(bx_s), int(by_s), blk


def encode_one(f: Path, out_dir: Path, args: argparse.Namespace) -> tuple[str, int, int, str]:
    m = FNAME_RE.match(f.name)
    if not m:
        return ("skip", 0, 0, "")
    tex_id = m.group(1).lower()
    w, h = int(m.group(2)), int(m.group(3))
    levels_hdr = int(m.group(4)) if m.group(4) else 1

    out_astcm = out_dir / f"{tex_id}.astcm"
    if out_astcm.exists() and not args.force:
        return ("skip", 0, 0, "")

    raw = f.read_bytes()
    mip_slices: list[tuple[int, int, bytes]] = []
    off = 0
    lw, lh = w, h
    for _ in range(levels_hdr):
        need = lw * lh * 4
        if off + need > len(raw):
            return ("err", 0, 0, f"[skip] {f.name}: truncated mip chain (need {off + need}, got {len(raw)})")
        mip_slices.append((lw, lh, raw[off : off + need]))
        off += need
        lw = max(1, lw >> 1)
        lh = max(1, lh >> 1)

    if levels_hdr == 1 and not args.no_mipmaps:
        cw, ch, cur = mip_slices[0]
        while min(cw, ch) >= 8 and len(mip_slices) < 8:
            cw, ch, cur = downsample_box2x2(cur, cw, ch)
            mip_slices.append((cw, ch, cur))

    bx, by, block_str = choose_block(w, h, args.block_small, args.block_large)
    payloads: list[bytes] = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp_png = Path(tmp) / "in.png"
        tmp_astc = Path(tmp) / "out.astc"
        for lv_idx, (mw, mh, mrgba) in enumerate(mip_slices):
            write_png(tmp_png, mw, mh, mrgba)
            cmd = [args.astcenc, "-cl", str(tmp_png), str(tmp_astc), block_str, args.quality, "-silent"]
            res = subprocess.run(cmd, capture_output=True, text=True)
            if res.returncode != 0:
                return ("err", 0, 0, f"[err] {f.name} L{lv_idx}: {res.stderr.strip()}")
            astc_blob = tmp_astc.read_bytes()
            if len(astc_blob) <= 16 or astc_blob[:4] != b"\x13\xab\xa1\x5c":
                return ("err", 0, 0, f"[err] {f.name} L{lv_idx}: invalid astcenc header")
            payloads.append(astc_blob[16:])

    tmp_out = out_dir / f"{tex_id}.astcm.tmp"
    with tmp_out.open("wb") as fp:
        fp.write(struct.pack("<4sBBBBHH", b"MTC1", bx, by, len(payloads), 0, w, h))
        for p in payloads:
            fp.write(struct.pack("<I", len(p)))
            fp.write(p)
    os.replace(tmp_out, out_astcm)
    return ("ok", len(raw), out_astcm.stat().st_size, "")


def main() -> int:
    ap = argparse.ArgumentParser(description="Pack Celosia raw RGBA texture dumps into Mali-G31 multi-mip .astcm cache files.")
    ap.add_argument("--in", dest="in_dir", required=True, help="Input directory containing .rgba dumps")
    ap.add_argument("--out", dest="out_dir", required=True, help="Output tex_cache directory for <hash>.astcm files")
    ap.add_argument("--astcenc", default="astcenc", help="Path to astcenc binary (from ARM astc-encoder)")
    ap.add_argument("--block-small", default="4x4", help="ASTC block size for textures < 128x128 (default: 4x4)")
    ap.add_argument("--block-large", default="6x6", help="ASTC block size for textures >= 128x128 (default: 6x6)")
    ap.add_argument("--quality", default="-medium", help="astcenc quality preset (default: -medium)")
    ap.add_argument("--no-mipmaps", action="store_true", help="Do not synthesize mipmaps for single-level dumps")
    ap.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 4), help="Parallel worker threads")
    ap.add_argument("--force", action="store_true", help="Re-encode existing .astcm files")
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
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futs = [pool.submit(encode_one, f, out_dir, args) for f in files]
        for fut in as_completed(futs):
            status, rb, ab, msg = fut.result()
            if msg:
                print(msg, file=sys.stderr)
            if status == "ok":
                ok += 1
                total_rgba_bytes += rb
                total_astc_bytes += ab
            else:
                skipped += 1

    print(f"Packed {ok} multi-mip ASTC textures (.astcm) into {out_dir} (skipped {skipped}).")
    if total_astc_bytes > 0:
        ratio = total_rgba_bytes / total_astc_bytes
        pct = (total_astc_bytes / total_rgba_bytes) * 100.0
        print(
            f"Size: rgba={total_rgba_bytes} bytes ({total_rgba_bytes / (1024 * 1024):.2f} MiB) "
            f"-> astcm={total_astc_bytes} bytes ({total_astc_bytes / (1024 * 1024):.2f} MiB) "
            f"({ratio:.2f}x, {pct:.1f}%)"
        )
    else:
        print(f"Size: rgba={total_rgba_bytes} bytes -> astcm={total_astc_bytes} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
