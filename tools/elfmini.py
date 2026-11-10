"""Minimal ELF32 little-endian reader (sections + program headers).

This module is a library, not a command line tool: the scripts in `tools/`
(`build_elf.py`, `funcmap_elf.py`, `gen_config.py`, `xref.py`, `callers.py`,
`disasm.py`) import it and read the boot ELF plus the merged `SOTC.elf`
through the `Elf` class.  Its public API (`Elf`, `Elf.read32`,
`Elf.exec_ranges`) and the attributes the importers read (`data`, `entry`,
`segments`, `sections`) are stability sensitive, so they must not change.
"""
import os
import struct

_ELF_MAGIC = b"\x7fELF"


class Elf:
    """Reads an ELF32 little-endian image: sections, segments, words.

    The whole file is read in binary mode on construction; `read32` maps a
    virtual address to a 32-bit little-endian word through the loaded
    segments, and `exec_ranges` lists the executable virtual address ranges.
    """

    def __init__(self, path):
        """Load and parse the ELF file at `path`.

        `path` may be a string or a `pathlib.Path`.  A file that cannot be
        read raises `OSError` (a `FileNotFoundError` naming the path when it
        is missing); a file that is too short or does not hold ELF data
        raises `ValueError` naming the file and the offset that could not be
        read.
        """
        path = os.fspath(path)
        try:
            with open(path, "rb") as handle:
                d = handle.read()
        except FileNotFoundError as exc:
            raise FileNotFoundError(f"ELF file not found: {path}") from exc
        except OSError as exc:
            raise OSError(f"cannot read ELF file {path}: {exc}") from exc

        if len(d) < 4:
            raise ValueError(f"{path}: truncated ELF data: cannot read 4 bytes at offset 0x0 (file is {len(d)} bytes)")
        if d[:4] != _ELF_MAGIC:
            raise ValueError(f"{path}: not an ELF file: bad magic {d[:4]!r} at offset 0x0")

        self.path = path
        self.data = d
        (self.entry, phoff, shoff) = self._unpack("<III", 0x18)
        phentsize, phnum, shentsize, shnum, shstrndx = self._unpack("<HHHHH", 0x2A)
        self.segments = []
        for i in range(phnum):
            ty, off, vaddr, paddr, filesz, memsz, flags, align = self._unpack("<8I", phoff + i * phentsize)
            if ty == 1:
                self.segments.append((vaddr, off, filesz, memsz, flags))
        self.sections = []
        if shoff and shnum:
            raw = [self._unpack("<10I", shoff + i * shentsize) for i in range(shnum)]
            stroff = raw[shstrndx][4]
            for r in raw:
                name = d[stroff + r[0]:d.index(b"\0", stroff + r[0])].decode()
                self.sections.append(dict(name=name, type=r[1], flags=r[2], addr=r[3], offset=r[4], size=r[5]))

    def _unpack(self, fmt, offset):
        """Unpack a `struct` format at `offset`, raising a `ValueError` naming the file when the data is short."""
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(self.data):
            raise ValueError(f"{self.path}: truncated ELF data: cannot read {size} bytes at offset {offset:#x} (file is {len(self.data)} bytes)")
        return struct.unpack_from(fmt, self.data, offset)

    def read32(self, addr):
        """Return the 32-bit little-endian word mapped at virtual address `addr`.

        Returns `None` when `addr` is not covered by a loaded segment.
        """
        for vaddr, off, filesz, memsz, flags in self.segments:
            if vaddr <= addr < vaddr + filesz:
                pos = off + addr - vaddr
                if pos + 4 > len(self.data):
                    raise ValueError(f"{self.path}: truncated ELF data: cannot read 4 bytes at offset {pos:#x} (file is {len(self.data)} bytes)")
                return struct.unpack_from("<I", self.data, pos)[0]
        return None

    def exec_ranges(self):
        """Return the executable ranges as a list of `(low, high)` virtual address tuples.

        The executable sections are used; when there are none, the executable
        segments are returned instead.
        """
        secs = [(s["addr"], s["addr"] + s["size"]) for s in self.sections if s["flags"] & 4 and s["size"]]
        return secs or [(v, v + fs) for v, o, fs, ms, fl in self.segments if fl & 1]
