#!/usr/bin/env python3
"""Turn the debug data of a YAFF image into an ELF debug file.

    yaff_debug_to_elf.py IMAGE_OR_DEBUG_FILE OUT.elf

The input is a YAFF image linked with -g (its blob sits at header.debug_offset)
or a separate debug file written by `strip -f` (the blob alone).  The output is
the ELF a host debugger reads: `gdb -ex 'add-symbol-file OUT.elf -s .text
CODE_BASE -s .data DATA_BASE ...'`, or readelf/objdump --dwarf.

The blob's section table already has the Elf32_Shdr layout (YaffDebugSection
in source/obj/tccyaff.h); this puts an ELF header in front of it and a
section-name table behind it, nothing else.
"""

import struct
import sys

YAFF_DEBUG_HEADER = "<4sHHIIIII"  # magic version count size sections names names_size crc
YAFF_DEBUG_SECTION = "<10I"
# debug_offset/debug_length close YaffHeader (YAFF_VERSION 6, 144 bytes).
YAFF_HEADER_DEBUG_OFFSET = 136


def find_blob(data):
    """The YDBG blob in a YAFF image or a separate debug file."""
    if data[:4] == b"YDBG":
        return data
    if data[:4] != b"YAFF":
        raise ValueError("neither a YAFF image nor a YAFF debug file")
    if data[7] < 6:
        raise ValueError(f"YAFF v{data[7]} carries no debug data (needs v6)")
    offset, length = struct.unpack_from("<II", data, YAFF_HEADER_DEBUG_OFFSET)
    if not offset:
        raise ValueError("the image has no debug data (linked without -g, or stripped)")
    return data[offset : offset + length]


def parse_blob(blob):
    magic, version, count, size, sections_offset, names_offset, names_size, crc = struct.unpack_from(
        YAFF_DEBUG_HEADER, blob, 0
    )
    if magic != b"YDBG" or version != 1:
        raise ValueError(f"bad debug blob (magic {magic!r}, version {version})")
    names = blob[names_offset : names_offset + names_size]
    sections = []
    for i in range(count):
        fields = struct.unpack_from(YAFF_DEBUG_SECTION, blob, sections_offset + i * 40)
        name_off = fields[0]
        name = names[name_off : names.index(b"\0", name_off)].decode()
        sections.append({"name": name, "fields": list(fields)})
    return {"sections": sections, "image_crc": crc, "size": size}


def to_elf(blob):
    info = parse_blob(blob)
    sections = info["sections"]
    SHT_NOBITS = 8
    out = bytearray(52)
    # Contents first, then the name table, then the section headers.
    placed = []
    for s in sections:
        name, type_, flags, addr, offset, size, link, inf, align, entsize = s["fields"]
        if type_ in (0, SHT_NOBITS):
            placed.append(0)
            continue
        while len(out) % 4:
            out.append(0)
        placed.append(len(out))
        out += blob[offset : offset + size]
    shstrtab = bytearray(b"\0")
    name_offsets = []
    for s in sections:
        if not s["name"]:
            name_offsets.append(0)
            continue
        name_offsets.append(len(shstrtab))
        shstrtab += s["name"].encode() + b"\0"
    shstr_name = len(shstrtab)
    shstrtab += b".shstrtab\0"
    shstr_offset = len(out)
    out += shstrtab
    while len(out) % 4:
        out.append(0)
    shoff = len(out)
    for s, file_offset, name_offset in zip(sections, placed, name_offsets):
        name, type_, flags, addr, offset, size, link, inf, align, entsize = s["fields"]
        if type_ == 0x80000001:  # YAFF_SHT_DEBUGLINK: keep it, as a note-like blob
            type_ = 0x80000001
        out += struct.pack("<10I", name_offset, type_, flags, addr, file_offset, size, link, inf, align, entsize)
    out += struct.pack("<10I", shstr_name, 3, 0, 0, shstr_offset, len(shstrtab), 0, 0, 1, 0)
    shnum = len(sections) + 1
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    # ET_EXEC, EM_ARM, EABI version 5 + soft/hard float left unset.
    struct.pack_into(
        "<16sHHIIIIIHHHHHH", out, 0, ident, 2, 40, 1, 0, 0, shoff, 0x05000000, 52, 0, 0, 40, shnum, shnum - 1
    )
    return bytes(out)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        data = f.read()
    try:
        elf = to_elf(find_blob(data))
    except ValueError as e:
        sys.stderr.write(f"{argv[1]}: {e}\n")
        return 1
    with open(argv[2], "wb") as f:
        f.write(elf)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
