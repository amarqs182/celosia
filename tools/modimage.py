"""Capture a loaded XFF module from a guest RAM dump.

After the game's loader runs, the in-RAM header has absolute section addresses
(memPt) and the symbol table holds final symbol addresses. The loader patches
only a few pointers, so the copy of the header found in RAM still matches the
file's own header almost byte for byte.

Typical use::

    ram = open("ee_ram.bin", "rb").read()
    mod = LoadedModule("mod/player.xff", ram, find_header("mod/player.xff", ram))
    for addr, (size, name) in mod.functions().items():
        ...

This module is a library helper; ``build_elf.py`` is its only in-tree consumer
and owns the command-line entry point.
"""
import struct
from dataclasses import dataclass
from pathlib import Path
from xff import Xff

RAM_MASK = 0x1FFFFFF
STT_FUNC = 2


def _checked_path(path):
    """Return ``path`` as a :class:`pathlib.Path`, or raise ``FileNotFoundError``.

    ``path`` may be a ``str`` or a :class:`pathlib.Path`; the error message
    names the offending file.
    """
    p = Path(path)
    if not p.is_file():
        raise FileNotFoundError(f"{p}: XFF module file not found")
    return p


def _checked_ram(ram):
    """Return ``ram`` unchanged, or raise ``TypeError`` if it is not bytes-like."""
    if not isinstance(ram, (bytes, bytearray)):
        raise TypeError(f"ram must be bytes or bytearray, not {type(ram).__name__}")
    return ram


@dataclass
class ModSection:
    """One loadable section of a loaded module.

    Attributes:
        name: section name as stored in the XFF file (e.g. ``".text"``).
        addr: absolute guest address of the section.
        size: size in bytes.
        data: section bytes, or ``None`` for ``NOBITS`` sections.
        exec: ``True`` for the executable (``.text``) section.
    """

    name: str
    addr: int
    size: int
    data: bytes | None   # None for NOBITS
    exec: bool


def find_header(xff_path, ram):
    """Locate a module's loaded xff2 header: the 16-byte aligned copy that best
    matches the file's own header (the loader only patches a few pointers).

    ``xff_path`` may be a ``str`` or a :class:`pathlib.Path`; ``ram`` is the raw
    guest memory dump as ``bytes``. Returns the header's offset within ``ram``.
    Raises ``FileNotFoundError`` when the XFF file is missing and ``ValueError``
    when no plausible header exists in the dump.
    """
    xff_path = _checked_path(xff_path)
    ram = _checked_ram(ram)
    head = open(xff_path, "rb").read()[:0x80]
    best, best_score = None, 0
    pos = ram.find(b"xff2")
    while pos >= 0:
        if pos % 16 == 0:
            score = sum(head[i:i + 4] == ram[pos + i:pos + i + 4] for i in range(0, len(head), 4))
            if score > best_score:
                best, best_score = pos, score
        pos = ram.find(b"xff2", pos + 4)
    if best is None or best_score < len(head) // 8:
        raise ValueError(f"{xff_path}: module header not found in RAM dump")
    return best


class LoadedModule:
    """A module's XFF header as it exists in guest RAM after loading.

    Attributes:
        xff: the parsed :class:`xff.Xff` file.
        name: file name of the module, without directories.
        base: guest address of the in-RAM ``xff2`` header.
        sections: :class:`ModSection` for every loaded section that carries
            bytes or reserves space.
        sect_addr: section index -> absolute guest address.
        symbols: ``(symbol, address)`` pairs with final guest addresses.
    """

    def __init__(self, xff_path, ram, header_addr):
        """Parse the loaded header found at ``header_addr`` in ``ram``.

        ``xff_path`` may be a ``str`` or a :class:`pathlib.Path`. Raises
        ``FileNotFoundError`` when the XFF file is missing and ``ValueError``
        when ``ram`` holds no ``xff2`` header at ``header_addr``.
        """
        name = xff_path.split("/")[-1] if isinstance(xff_path, str) else Path(xff_path).name
        xff_path = _checked_path(xff_path)
        ram = _checked_ram(ram)
        if header_addr < 0:
            raise ValueError(f"negative header address: {header_addr:#x}")
        self.xff = x = Xff(str(xff_path))
        self.name = name
        self.base = header_addr
        u32 = lambda a: struct.unpack_from("<I", ram, a & RAM_MASK)[0]
        if ram[header_addr:header_addr + 4] != b"xff2":
            raise ValueError(f"no xff2 header at {header_addr:#x}")
        self.sections = []
        self.sect_addr = {}
        for s in x.sections:
            mem = u32(header_addr + x.sect_off + s.index * 0x20)
            self.sect_addr[s.index] = mem
            if not s.size or s.type not in (1, 8, 0x7FFFF420) or s.name.startswith(".DVP") or s.name == ".reginfo":
                continue
            nobits = s.type == 8
            data = None if nobits else bytes(ram[mem & RAM_MASK:(mem & RAM_MASK) + s.size])
            self.sections.append(ModSection(s.name, mem, s.size, data, s.name == ".text"))
        self.symbols = []
        for s in x.symbols:
            addr = u32(header_addr + x.sym_off + s.index * 0x10 + 4)
            self.symbols.append((s, addr))

    def text(self):
        """Return the executable (``.text``) section.

        Raises ``StopIteration`` when the module has no executable section.
        """
        return next(s for s in self.sections if s.exec)

    def functions(self):
        """(addr, size, name) for every function symbol defined in .text."""
        t = self.text()
        out = {}
        for s, addr in self.symbols:
            if s.type == STT_FUNC and s.name and t.addr <= addr < t.addr + t.size:
                if addr not in out or (s.bind and not out[addr][2]):
                    out[addr] = (s.size, s.name, s.bind)
        return {a: (sz, n) for a, (sz, n, b) in out.items()}

    def data_symbols(self):
        """Map guest address -> name for defined data (non-ABS) symbols."""
        return {addr: s.name for s, addr in self.symbols
                if s.name and s.type == 1 and s.shndx not in (0, 0xFFF1)}
