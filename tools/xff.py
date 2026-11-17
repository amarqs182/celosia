"""Parser for Team Ico's XFF2 relocatable module format (Shadow of the Colossus).

An XFF2 module (``*.XFF``) is a self-describing, relocatable, ELF-like image that
the game's loader maps into EE RAM and then fixes up. The file begins with the
4-byte magic ``b"xff2"`` followed by a header of little-endian u32 fields; the
header points at five tables, and every pointer stored in the file is a *file
offset*:

    0x0C  unknown word (version/flags)        0x50  import table offset
    0x14  total file size                     0x54  symbol table offset
    0x1C  import count                        0x58  string table offset
    0x24  symbol count                        0x5C  section table offset
    0x38  relocation table count              0x64  relocation table offset
    0x40  section count                       0x68  section-name offset table
    0x44  entry pointer (unused)              0x6C  section-name string base

Sections are 0x20-byte ELF ``shdr`` records, symbols are 0x10-byte ELF ``sym``
records, and each relocation table is a 0x1C-byte descriptor followed by two
parallel arrays of u32 pairs: (offset, r_info) and (addend, padding). Inside a
descriptor the ``*_rel`` fields are file offsets while the non-``_rel`` twins are
pointers the loader patches, so when reading a module straight from disc only the
``*_rel`` fields are meaningful.

Layout based on research from github.com/Fantaskink/SOTC (tools/parse_xff_relocs.py).

Public API
    ``Xff(path)`` reads a module read-only from disk and parses it. Attributes:
    header words (``hdr_unk0c``, ``file_size``, ``imp_count``, ``sym_count``,
    ``reloc_count``, ``sect_count``, ``entry_ptr``, ``imp_off``, ``sym_off``,
    ``str_off``, ``sect_off``, ``reloc_off``, ``sname_offs``, ``sname_base``),
    the raw ``data`` bytes, and the parsed ``sections``, ``symbols``, ``imports``
    and ``reloc_tables`` lists (holding ``Section``, ``Symbol``, ``RelocTable``
    and ``Reloc`` instances). Helpers: ``cstr(offset)`` and
    ``section_bytes(section)``.

    Every error raised by ``Xff`` names the offending file: ``FileNotFoundError``
    or ``OSError`` when the file cannot be read, and ``ValueError`` for a wrong
    magic or for a header/table/string that falls outside the file (truncated
    module). The message reports the offset and the structure that was expected.
"""
import struct
from dataclasses import dataclass, field

R_MIPS_NONE, R_MIPS_16, R_MIPS_32, R_MIPS_REL32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = range(7)
SHN_ABS = 0xFFF1
SHN_COMMON = 0xFFF2


@dataclass
class Section:
    """One section header (ELF ``shdr``) of an XFF2 module."""

    index: int
    name: str
    mem_pt: int
    file_pt: int
    size: int
    align: int
    type: int      # ELF sh_type (1 = PROGBITS, 8 = NOBITS)
    flags: int     # ELF sh_flags
    moved: int
    offs: int      # file offset of contents (bss: none)


@dataclass
class Symbol:
    """One symbol table entry (ELF ``sym``) of an XFF2 module."""

    index: int
    name: str
    value: int
    size: int
    type: int      # STT_*
    bind: int      # STB_*
    other: int
    shndx: int


@dataclass
class Reloc:
    """One relocation: where it applies, its R_MIPS_* type and its addend."""

    offset: int    # offset within target section
    type: int
    sym: int
    addend: int    # original instruction/word stored by the converter


@dataclass
class RelocTable:
    """A group of relocations that apply to a single section."""

    type: int
    sect: int
    relocs: list = field(default_factory=list)


class Xff:
    """Parsed XFF2 module: header words plus its sections, symbols and relocs."""

    # Bytes of the header block that Xff reads (last field ends at 0x70).
    _HEADER_SIZE = 0x70

    def __init__(self, path):
        """Read and parse the XFF2 module at ``path`` (read-only, binary)."""
        self.path = path
        try:
            with open(path, "rb") as fp:
                self.data = fp.read()
        except FileNotFoundError as exc:
            raise FileNotFoundError(f"{path}: cannot open XFF2 module: file not found") from exc
        except OSError as exc:
            raise OSError(f"{path}: cannot read XFF2 module: {exc}") from exc
        d = self.data
        self._need(0x0, 4, "XFF2 magic")
        if d[:4] != b"xff2":
            raise ValueError(
                f"{path}: not an XFF2 module: expected magic b'xff2' at offset 0x0, "
                f"found {bytes(d[:4])!r}")
        self._need(0x0, self._HEADER_SIZE, "XFF2 header")
        u = lambda o: struct.unpack_from("<I", d, o)[0]
        self.u32 = u
        self.hdr_unk0c = u(0x0C)
        self.file_size = u(0x14)
        self.imp_count = u(0x1C)
        self.sym_count = u(0x24)
        self.reloc_count = u(0x38)
        self.sect_count = u(0x40)
        self.entry_ptr = u(0x44) if False else None
        self.imp_off = u(0x50)
        self.sym_off = u(0x54)
        self.str_off = u(0x58)
        self.sect_off = u(0x5C)
        self.reloc_off = u(0x64)
        self.sname_offs = u(0x68)
        self.sname_base = u(0x6C)

        if self.sect_count:
            self._need(self.sect_off, self.sect_count * 0x20, "section table")
            self._need(self.sname_offs, self.sect_count * 4, "section name offset table")
        self.sections = []
        for i in range(self.sect_count):
            o = self.sect_off + i * 0x20
            mem, fpt, size, align, ty, fl, moved, offs = struct.unpack_from("<8I", d, o)
            name = self.cstr(self.sname_base + u(self.sname_offs + i * 4))
            if size and ty != 8:
                self._need(offs, size, f"contents of section {i} ({name})")
            self.sections.append(Section(i, name, mem, fpt, size, align, ty, fl, moved, offs))

        if self.sym_count:
            self._need(self.sym_off, self.sym_count * 0x10, "symbol table")
        self.symbols = []
        for i in range(self.sym_count):
            o = self.sym_off + i * 0x10
            no, val, size, info, other, shndx = struct.unpack_from("<IIIBBH", d, o)
            self.symbols.append(Symbol(i, self.cstr(self.str_off + no), val, size,
                                       info & 0xF, info >> 4, other, shndx))

        if self.imp_count:
            self._need(self.imp_off, self.imp_count * 4, "import table")
        self.imports = [u(self.imp_off + i * 4) for i in range(self.imp_count)]

        if self.reloc_count:
            self._need(self.reloc_off, self.reloc_count * 0x1C, "relocation table descriptors")
        self.reloc_tables = []
        for i in range(self.reloc_count):
            o = self.reloc_off + i * 0x1C
            ty, n, sect, _a, _b, addr_rel, inst_rel = struct.unpack_from("<7I", d, o)
            if n:
                what = f"relocation table {i} entries"
                self._need(addr_rel, n * 8, what)
                self._need(inst_rel, n * 8, what)
            t = RelocTable(ty, sect)
            for j in range(n):
                off, info = struct.unpack_from("<II", d, addr_rel + j * 8)
                add, _ = struct.unpack_from("<II", d, inst_rel + j * 8)
                t.relocs.append(Reloc(off, info & 0xFF, info >> 8, add))
            self.reloc_tables.append(t)

    def _need(self, offset, size, what):
        """Check that ``size`` bytes at file offset ``offset`` exist in the module."""
        end = offset + size
        if offset < 0 or end > len(self.data):
            raise ValueError(
                f"{self.path}: truncated XFF2 module: {what} needs {size:#x} bytes at "
                f"offset {offset:#x}, but the file is only {len(self.data):#x} bytes")

    def cstr(self, o):
        """Decode the NUL-terminated latin-1 string that starts at file offset ``o``."""
        try:
            e = self.data.index(b"\0", o)
        except ValueError:
            raise ValueError(
                f"{self.path}: truncated XFF2 module: unterminated string at offset {o:#x} "
                f"(file is {len(self.data):#x} bytes)") from None
        return self.data[o:e].decode("latin-1")

    def section_bytes(self, s):
        """Return the contents of ``s`` (zero-filled bytes for NOBITS sections)."""
        if s.type == 8:
            return bytes(s.size)
        return self.data[s.offs:s.offs + s.size]
