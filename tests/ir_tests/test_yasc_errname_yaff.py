"""yasc error names in a YAFF executable (what the device links).

Every yasc file records the error names it uses in its yasc_errname section,
and libtcc1's __yasc_error_name scans them from __start_yasc_errname to
__stop_yasc_errname.  A YAFF module only has .text/.rodata/.data/.bss, and
the writer refused the section: on the device `tcc -ccompat yasc_errors.c
yasc_errors+abi.c` failed with "YAFF output cannot represent allocated
section 'yasc_errname'".  The host suite links ELF and never saw it.

The linker now moves the records to the end of .rodata for a YAFF output and
defines the two symbols there (elf_output_file, yaff_merge_yasc_errname).
Here: the two programs of test_qemu.py link as YAFF (-g, so the image carries
its full symbol table), the symbols bound the records, the bytes between them
are every object's records in link order, and a lookup done the way
__yasc_error_name does it finds the names the .expect files print.  Running
them needs a YasOS guest; the records and their bounds are what changed.
"""

import struct
import subprocess
import sys
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
ROOT = CURRENT_DIR.parent.parent
TCC = ROOT / "armv8m-tcc"
LIBTCC1 = ROOT / "armv8m-libtcc1.a"
TO_ELF = ROOT / "scripts" / "yaff_debug_to_elf.py"

pytestmark = pytest.mark.skipif(not TCC.exists() or not LIBTCC1.exists(), reason="needs make cross")

# YaffHeader (source/obj/tccyaff.h, packed) fields read here
CODE_LENGTH = 8
PLT_LENGTH = 68
TEXT_OFFSET = 92

# errno values of the YasOS libc (newlib numbering)
EPERM, ENOENT, EACCES, EINVAL = 1, 2, 13, 22

PROGRAMS = {
    "yasc_errors": ["yasc_errors.c", "yasc_errors+abi.c"],
    "yasc_error_name": ["yasc_error_name.c", "yasc_error_name+b.c"],
}


def _sections(elf):
    """name -> (sh_type, sh_addr, sh_offset, sh_size, sh_link) of an ELF32 file"""
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    raw = [struct.unpack_from("<IIIIIIIIII", elf, shoff + i * shentsize) for i in range(shnum)]
    strtab_off = raw[shstrndx][4]
    out = {}
    for i, (name, typ, _flags, addr, off, size, link, _info, _align, _es) in enumerate(raw):
        n = elf[strtab_off + name : elf.index(b"\0", strtab_off + name)].decode()
        out[n] = (typ, addr, off, size, link, i)
    return out, raw


def _symbols(elf):
    secs, raw = _sections(elf)
    _typ, _addr, off, size, link, _i = secs[".symtab"]
    stroff = raw[link][4]
    syms = {}
    for k in range(size // 16):
        name, value, _sz, _info, _other, _shndx = struct.unpack_from("<IIIBBH", elf, off + k * 16)
        n = elf[stroff + name : elf.index(b"\0", stroff + name)].decode()
        syms[n] = value
    return syms, secs


def _records(block):
    """[(value, flags, name)] the way lib/yasc.c walks them"""
    out, p = [], 0
    while p + 4 <= len(block):
        n = block[p + 3]
        if n:
            out.append((block[p] | block[p + 1] << 8, block[p + 2], block[p + 4 : p + 4 + n].decode()))
        p += (4 + n + 1 + 3) & ~3
    return out


def _error_name(records, e):
    """lib/yasc.c's __yasc_error_name: an errno name wins over an alias"""
    alias = None
    for value, flags, name in records:
        if value != e:
            continue
        if flags == 2:
            return name
        alias = alias or name
    return alias or ("(unknown error)" if e else "(no error)")


def _object_records(tmp_path, src, opt):
    obj = tmp_path / (Path(src).stem + opt + ".o")
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-c", opt, str(CURRENT_DIR / src), "-o", str(obj)], capture_output=True, text=True
    )
    assert proc.returncode == 0, proc.stderr
    elf = obj.read_bytes()
    secs, _raw = _sections(elf)
    if "yasc_errname" not in secs:
        return b""  # a file that names no error
    _typ, _addr, off, size, _link, _i = secs["yasc_errname"]
    return elf[off : off + size]


def _link_yaff(tmp_path, name, *flags):
    out = tmp_path / f"{name}.yaff"
    cmd = [str(TCC), "-ccompat", "-g", "-Wl,-oformat=yaff", *flags]
    cmd += [str(CURRENT_DIR / s) for s in PROGRAMS[name]]
    # this checkout's runtime: __yasc_error_name is in its libtcc1 (the
    # sysroot's may predate it)
    cmd += [str(LIBTCC1), "-o", str(out)]
    return subprocess.run(cmd, capture_output=True, text=True), out


@pytest.fixture(scope="module")
def can_link(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("probe")
    (tmp / "p.c").write_text('#include <stdio.h>\nint main(void) { puts("x"); return 0; }\n')
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-Wl,-oformat=yaff", str(tmp / "p.c"), "-o", str(tmp / "p")],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        pytest.skip(f"no YasOS sysroot to link against:\n{proc.stderr}")


@pytest.mark.parametrize("flags", [["-O0"], ["-O2"], ["-O1", "-Wl,--gc-sections"]], ids=["O0", "O2", "O1-gc"])
@pytest.mark.parametrize("name", sorted(PROGRAMS))
def test_yasc_error_names_link_as_yaff(tmp_path, can_link, name, flags):
    proc, out = _link_yaff(tmp_path, name, *flags)
    assert proc.returncode == 0, f"YAFF link failed:\n{proc.stderr}"
    image = out.read_bytes()
    assert image[:4] == b"YAFF"

    elf_path = tmp_path / f"{name}.elf"
    conv = subprocess.run([sys.executable, str(TO_ELF), str(out), str(elf_path)], capture_output=True, text=True)
    assert conv.returncode == 0, conv.stderr
    syms, secs = _symbols(elf_path.read_bytes())
    start, stop = syms["__start_yasc_errname"], syms["__stop_yasc_errname"]
    _t, rodata_addr, _o, rodata_size, _l, _i = secs[".rodata"]
    assert rodata_addr <= start < stop <= rodata_addr + rodata_size, (hex(start), hex(stop), hex(rodata_addr))
    assert start % 4 == 0

    # .rodata follows .text and .plt in the file
    code_length, = struct.unpack_from("<I", image, CODE_LENGTH)
    plt_length, = struct.unpack_from("<I", image, PLT_LENGTH)
    text_offset, = struct.unpack_from("<I", image, TEXT_OFFSET)
    at = text_offset + code_length + plt_length + (start - rodata_addr)
    block = image[at : at + (stop - start)]

    opt = flags[0]
    expected = b"".join(_object_records(tmp_path, s, opt) for s in PROGRAMS[name])
    assert block == expected, "the records between __start/__stop_yasc_errname are not the objects' records"

    records = _records(block)
    names = {n for _v, _f, n in records}
    if name == "yasc_error_name":
        # yasc_error_name.expect
        assert _error_name(records, ENOENT) == "ENOENT"  # errno wins over FileNotFound = ENOENT
        assert _error_name(records, EPERM) == "EPERM"  # named in no file: from errno.h
        assert _error_name(records, EACCES) == "EACCES"
        assert _error_name(records, EINVAL) == "EINVAL"
        for n in ("BadPath", "OnlyInFileB"):
            v = next(v for v, f, nm in records if nm == n)
            assert _error_name(records, v) == n
    else:
        for n in ("BadDigit", "Overflow", "Empty", "FileNotFound", "BadPath"):
            assert n in names, (n, sorted(names))
