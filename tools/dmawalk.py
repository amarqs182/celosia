"""Walk a VIF1 DMA chain in a RAM dump and decode VIF codes and GIF packets.

Input: a RAM dump -- a flat image of EE RAM, either a raw memory dump or an
ELF image whose bytes are laid out as raw memory (ELF containers are not
parsed here).
Output: the DMA walk report on stdout: the walked tag and byte counts, the
decoded GIF packets and the VIF command histogram.

Usage:
    python tools/dmawalk.py <ram-dump> <start-address-hex>
"""
import argparse
import os
import struct
import sys


def _hex_address(text):
    """Parse `text` as a hexadecimal address, reporting a clean argparse error."""
    try:
        return int(text, 16)
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid hexadecimal address: {text!r}") from None


parser = argparse.ArgumentParser(
    description="Walk a VIF1 DMA chain in a RAM dump and decode VIF codes and GIF packets.",
)
parser.add_argument("dump", help="RAM dump (or ELF image laid out as raw memory) to read")
parser.add_argument("start", type=_hex_address, help="start address of the DMA chain, in hex")
# Extra arguments have always been ignored by this tool; keep accepting them.
args, _ = parser.parse_known_args()

if not os.path.isfile(args.dump):
    print(f"Error: file not found: {args.dump}", file=sys.stderr)
    sys.exit(1)

with open(args.dump, "rb") as handle:  # raw bytes: no text encoding applies
    ram = handle.read()
start = args.start
u64 = lambda a: struct.unpack_from("<Q", ram, a & 0x1FFFFFF)[0]

def walk(tadr):
    out, asr, n = bytearray(), [], 0
    while n < 8192:
        tag = u64(tadr); qwc = tag & 0xFFFF; tid = (tag >> 28) & 7; addr = (tag >> 32) & 0x7FFFFFFF
        out += ram[(tadr + 8) & 0x1FFFFFF:(tadr + 16) & 0x1FFFFFF]  # TTE: VIF codes in tag upper half
        n += 1
        if tid == 0: data, tadr, end = addr, tadr + 16, True
        elif tid == 1: data = tadr + 16; tadr, end = data + qwc * 16, False
        elif tid == 2: data, tadr, end = tadr + 16, addr, False
        elif tid in (3, 4): data, tadr, end = addr, tadr + 16, False
        elif tid == 5: data = tadr + 16; asr.append(data + qwc * 16); tadr, end = addr, False
        elif tid == 6:
            data = tadr + 16
            if asr: tadr, end = asr.pop(), False
            else: end = True
        else: data, end = tadr + 16, True
        if data & 0x80000000:  # scratchpad source: not in dump
            out += bytes(qwc * 16)
        else:
            out += ram[data & 0x1FFFFFF:(data & 0x1FFFFFF) + qwc * 16]
        if end: break
    return bytes(out), n

def gif(pkt, path):
    pos = 0
    while pos + 16 <= len(pkt):
        lo, hi = struct.unpack_from("<QQ", pkt, pos); pos += 16
        nloop = lo & 0x7FFF; eop = (lo >> 15) & 1; flg = (lo >> 58) & 3; nreg = (lo >> 60) & 15 or 16
        if flg == 0:
            regs = [(hi >> (4 * i)) & 15 for i in range(nreg)]
            for _ in range(nloop):
                for r in regs:
                    if r == 0xE and pos + 16 <= len(pkt):
                        data, reg = struct.unpack_from("<QQ", pkt, pos)
                        reg &= 0xFF
                        if reg == 0x50:
                            print(f"  {path} BITBLTBUF dbp={(data>>32)&0x3FFF} dbw={(data>>48)&0x3F} dpsm={(data>>56)&0x3F:#x}")
                        elif reg in (6, 7):
                            print(f"  {path} TEX0_{reg-5} tbp={data&0x3FFF} psm={(data>>20)&0x3F:#x} cbp={(data>>37)&0x3FFF}")
                        elif reg == 0x53:
                            print(f"  {path} TRXDIR {data&3}")
                    pos += 16
        elif flg == 1:
            pos += ((nloop * nreg + 1) // 2) * 16
        else:
            print(f"  {path} IMAGE {nloop} qw"); pos += nloop * 16
        if eop: return pos
    return pos

buf, ntags = walk(start)
print(f"tags={ntags} bytes={len(buf)}")
pos = 0
counts = {}
while pos + 4 <= len(buf):
    code = struct.unpack_from("<I", buf, pos)[0]; pos += 4
    cmd = (code >> 24) & 0x7F; imm = code & 0xFFFF; num = (code >> 16) & 0xFF
    counts[cmd] = counts.get(cmd, 0) + 1
    if cmd in (0x50, 0x51):
        pos = (pos + 15) & ~15
        q = imm or 65536
        gif(buf[pos:pos + q * 16], "DIRECT")
        pos += q * 16
    elif cmd == 0x4A:  # MPG
        pos += (num or 256) * 8
    elif cmd in (0x20,): pos += 4         # STMASK
    elif cmd in (0x30, 0x31): pos += 16   # STROW/STCOL
    elif cmd >= 0x60:  # UNPACK
        vn, vl = (cmd >> 2) & 3, cmd & 3
        bits = [32, 16, 8, 4 if vn == 3 else 16][vl]
        n = num or 256
        size = (bits * (vn + 1) * n + 31) // 32 * 4
        pos += size
print("vif cmds:", {hex(k): v for k, v in sorted(counts.items())})
