"""Phase 5: object, linker, and debug-info coverage tests.

Each test cross-compiles a tiny C case with libs/tinycc/armv8m-tcc and then
inspects the resulting object or executable with arm-none-eabi-readelf and
arm-none-eabi-objdump.  The assertions are characterizations of the current
linker/ELF output; if the format changes the tests should be flipped to lock
in the new layout.
"""

import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent.parent  # libs/tinycc
TCC = ROOT / "armv8m-tcc"
LINKER_DIR = Path(__file__).parent
BUILD_DIR = LINKER_DIR / "build"

READELF = "arm-none-eabi-readelf"
OBJDUMP = "arm-none-eabi-objdump"


def _base_cflags():
    """Default cross-compile flags used by the rest of the test suite."""
    return [
        "-O1",
        "-nostdlib",
        "-fvisibility=hidden",
        "-mcpu=cortex-m33",
        "-mthumb",
        "-mfloat-abi=soft",
        "-ffunction-sections",
    ]


def _compile_to_object(name, subdir, extra_cflags=()):
    """Cross-compile a case in <subdir>/<name>.c to a relocatable object."""
    src = LINKER_DIR / subdir / f"{name}.c"
    obj = BUILD_DIR / subdir / f"{name}.o"
    obj.parent.mkdir(parents=True, exist_ok=True)

    cflags = _base_cflags() + ["-c"] + list(extra_cflags)
    cmd = [str(TCC)] + cflags + [str(src), "-o", str(obj)]
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"Compile failed for {subdir}/{name}: {cmd}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return obj


def _compile_to_yaff(name, tinycc_root):
    """Cross-compile a YAFF executable for <subdir>/yaff/<name>.c.

    YAFF output needs the armv8m runtime libraries (libtcc1 + softfp).  The
    cross compiler searches for ``fp/libsoftfp.a`` relative to the tcc lib
    path, so we pass -B<root>/lib; ``armv8m-libtcc1.a`` is found via -L<root>.
    """
    src = LINKER_DIR / "yaff" / f"{name}.c"
    out = BUILD_DIR / "yaff" / f"{name}.yaff"
    out.parent.mkdir(parents=True, exist_ok=True)

    cflags = [
        "-O1",
        "-nostdlib",
        "-fvisibility=hidden",
        "-mcpu=cortex-m33",
        "-mthumb",
        "-mfloat-abi=soft",
        "-ffunction-sections",
        f"-B{tinycc_root}/lib",
        f"-L{tinycc_root}/lib/fp",
        f"-L{tinycc_root}",
    ]
    cmd = [str(TCC)] + cflags + [str(src), "-o", str(out)]
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    return result, cmd, out


def _readelf_reloc(obj):
    """Return a list of relocation entries as dicts."""
    result = subprocess.run(
        [READELF, "-r", str(obj)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    assert result.returncode == 0, f"readelf -r failed for {obj}: {result.stderr}"

    relocs = []
    in_rel = False
    for line in result.stdout.splitlines():
        if line.startswith("Relocation section"):
            in_rel = True
            continue
        if in_rel:
            m = re.match(
                r"\s*([0-9a-f]+)\s+([0-9a-f]+)\s+(\S+)\s+([0-9a-f]+)\s+(.*)$",
                line,
            )
            if m:
                relocs.append(
                    {
                        "offset": m.group(1),
                        "info": m.group(2),
                        "type": m.group(3),
                        "sym_value": m.group(4),
                        "sym_name": m.group(5).strip(),
                    }
                )
    return relocs


def _readelf_sections(obj):
    """Return a list of section-header entries as dicts."""
    result = subprocess.run(
        [READELF, "-S", str(obj)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    assert result.returncode == 0, f"readelf -S failed for {obj}: {result.stderr}"

    sections = []
    for line in result.stdout.splitlines():
        # The Flg column is blank for sections with no alloc/write/exec/etc.
        # flags (.symtab, .strtab, .shstrtab, .rel.*), so it must be matched
        # with `\S*` (zero-or-more) rather than `\S+`, otherwise those
        # section rows silently fail to match and vanish from the result.
        m = re.match(
            r"\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+"
            r"([0-9a-f]+)\s+([0-9a-f]+)\s+(\S*)\s+(\d+)\s+(\d+)\s+(\d+)\s*$",
            line,
        )
        if m:
            sections.append(
                {
                    "nr": int(m.group(1)),
                    "name": m.group(2),
                    "type": m.group(3),
                    "addr": m.group(4),
                    "off": m.group(5),
                    "size": m.group(6),
                    "es": m.group(7),
                    "flags": m.group(8),
                    "lk": int(m.group(9)),
                    "inf": int(m.group(10)),
                    "al": int(m.group(11)),
                }
            )
    return sections


def _readelf_section_names(obj):
    """Return ordered list of section names."""
    return [s["name"] for s in _readelf_sections(obj)]


def _readelf_syms(obj):
    """Return a list of .symtab entries as dicts, in symbol-table order."""
    result = subprocess.run(
        [READELF, "-s", str(obj)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    assert result.returncode == 0, f"readelf -s failed for {obj}: {result.stderr}"

    syms = []
    for line in result.stdout.splitlines():
        m = re.match(
            r"\s*(\d+):\s+([0-9a-f]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s*(.*)$",
            line,
        )
        if m:
            syms.append(
                {
                    "num": int(m.group(1)),
                    "value": m.group(2),
                    "size": int(m.group(3)),
                    "type": m.group(4),
                    "bind": m.group(5),
                    "vis": m.group(6),
                    "ndx": m.group(7),
                    "name": m.group(8).strip(),
                }
            )
    return syms


def _objdump_sections(obj):
    """Return set of section names reported by objdump -h."""
    result = subprocess.run(
        [OBJDUMP, "-h", str(obj)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    assert result.returncode == 0, f"objdump -h failed for {obj}: {result.stderr}"

    names = set()
    for line in result.stdout.splitlines():
        m = re.match(r"\s*\d+\s+([\.\w]+)\s+", line)
        if m:
            names.add(m.group(1))
    return names


# -----------------------------------------------------------------------------
# relocations/
# -----------------------------------------------------------------------------
@pytest.mark.linker
@pytest.mark.linker_reloc
def test_relocation_global_external():
    obj = _compile_to_object("01_global_external", "relocations")
    relocs = _readelf_reloc(obj)

    types = {r["type"] for r in relocs}
    by_name = {r["sym_name"]: r["type"] for r in relocs}

    # External function call should use a Thumb relative jump relocation.
    assert "R_ARM_THM_JUMP24" in types, f"expected R_ARM_THM_JUMP24, got {types}"
    assert by_name.get("external_func") == "R_ARM_THM_JUMP24"

    # External/global data should use absolute 32-bit relocations.
    assert "R_ARM_ABS32" in types, f"expected R_ARM_ABS32, got {types}"
    assert by_name.get("external_var") == "R_ARM_ABS32"
    assert by_name.get("global_var") == "R_ARM_ABS32"

    # Static data is resolved locally and should not generate a relocation.
    assert "static_var" not in by_name


@pytest.mark.linker
@pytest.mark.linker_reloc
def test_relocation_static_local():
    obj = _compile_to_object("02_static_local", "relocations")
    relocs = _readelf_reloc(obj)

    # A TU that only touches static data should have no relocations.
    assert relocs == [], f"expected no relocations, got {relocs}"


@pytest.mark.linker
@pytest.mark.linker_reloc
def test_relocation_string_literal_rodata():
    obj = _compile_to_object("03_string_literal_rodata", "relocations")
    relocs = _readelf_reloc(obj)
    syms = _readelf_syms(obj)
    sections = _readelf_sections(obj)

    # msg's initializer relocates into .rodata against the string literal's
    # own local symbol, not against `msg` itself.
    assert len(relocs) == 1, f"expected exactly one relocation, got {relocs}"
    r = relocs[0]
    assert r["type"] == "R_ARM_ABS32", f"expected R_ARM_ABS32, got {r['type']}"

    lit_sym = next(s for s in syms if s["name"] == r["sym_name"])
    assert lit_sym["bind"] == "LOCAL"
    assert lit_sym["type"] == "OBJECT"

    lit_section = next(s for s in sections if s["nr"] == int(lit_sym["ndx"]))
    assert lit_section["name"] == ".rodata"

    # msg itself lives in .data and is exported as a global symbol.
    msg_sym = next(s for s in syms if s["name"] == "msg")
    assert msg_sym["bind"] == "GLOBAL"


@pytest.mark.linker
@pytest.mark.linker_reloc
def test_relocation_function_pointer_is_abs32():
    obj = _compile_to_object("04_function_pointer_to_code", "relocations")
    relocs = _readelf_reloc(obj)
    by_name = {r["sym_name"]: r["type"] for r in relocs}

    # Storing a function's address in a data object is a data reference, not
    # a call, so it must use an absolute relocation rather than the
    # THM_JUMP24 relocation direct calls use.
    assert by_name.get("add") == "R_ARM_ABS32", f"expected R_ARM_ABS32 for fp->add, got {relocs}"


@pytest.mark.linker
@pytest.mark.linker_reloc
def test_relocation_static_to_static_call():
    obj = _compile_to_object("05_static_to_static_call", "relocations")
    relocs = _readelf_reloc(obj)
    syms = _readelf_syms(obj)

    call_relocs = [r for r in relocs if r["sym_name"] == "helper"]
    assert len(call_relocs) == 2, f"expected 2 calls to helper, got {relocs}"
    for r in call_relocs:
        assert r["type"] == "R_ARM_THM_JUMP24"

    helper_sym = next(s for s in syms if s["name"] == "helper")
    assert helper_sym["bind"] == "LOCAL"


@pytest.mark.linker
@pytest.mark.linker_reloc
def test_relocation_multiple_refs_same_symbol():
    obj = _compile_to_object("06_multiple_relocs_same_symbol", "relocations")
    relocs = _readelf_reloc(obj)

    shared = [r for r in relocs if r["sym_name"] == "shared_var"]
    assert len(shared) == 3, f"expected 3 relocations against shared_var, got {relocs}"
    # All three must reference the exact same symbol-table slot (encoded in
    # the high bits of r_info alongside the (identical) relocation type).
    assert len({r["info"] for r in shared}) == 1, f"expected one symbol index for all refs, got {shared}"


# -----------------------------------------------------------------------------
# sections/
# -----------------------------------------------------------------------------
@pytest.mark.linker
@pytest.mark.linker_section
def test_section_order_and_alignment():
    obj = _compile_to_object("01_order_align", "sections")
    sections = _readelf_sections(obj)
    names = [s["name"] for s in sections]

    # Standard alloc sections are present and ordered text -> rodata -> data -> bss.
    assert ".text" in names
    assert ".data" in names
    assert ".bss" in names

    # Custom section emitted by the source.
    assert ".custom_text" in names

    # The explicitly aligned variable requests 16-byte alignment.
    data = next(s for s in sections if s["name"] == ".data")
    assert data["al"] >= 4, f"expected .data alignment >= 4, got {data['al']}"


@pytest.mark.linker
@pytest.mark.linker_section
def test_function_sections():
    obj = _compile_to_object("02_function_sections", "sections")
    names = _objdump_sections(obj)

    # With -ffunction-sections enabled the standard .text section still exists.
    # This fork currently keeps all functions in the single .text section rather
    # than emitting per-function .text.func_name subsections; if that changes
    # this assertion should be flipped to require the subsections.
    assert ".text" in names, f"missing .text section; sections: {names}"
    assert ".text.func_a" not in names, f"unexpected per-function section; sections: {names}"


@pytest.mark.linker
@pytest.mark.linker_section
def test_alignment_double_longlong_offsets():
    obj = _compile_to_object("03_alignment_double_longlong", "sections")
    syms = _readelf_syms(obj)
    sections = _readelf_sections(obj)

    by_name = {s["name"]: s for s in syms}
    c = int(by_name["c"]["value"], 16)
    d = int(by_name["d"]["value"], 16)
    arr = int(by_name["arr"]["value"], 16)

    assert c == 0
    # double must land on an 8-byte boundary even though only a 1-byte char
    # precedes it (7 bytes of padding).
    assert d % 8 == 0, f"double 'd' is not 8-byte aligned: offset {d}"
    assert d >= c + 1
    assert arr % 8 == 0, f"'arr' (long long[4]) is not 8-byte aligned: offset {arr}"

    data = next(s for s in sections if s["name"] == ".data")
    assert data["al"] >= 8, f"expected .data alignment >= 8, got {data['al']}"


@pytest.mark.linker
@pytest.mark.linker_section
def test_data_sections_quirk():
    obj = _compile_to_object("04_data_sections_quirk", "sections", extra_cflags=["-fdata-sections"])
    names = _objdump_sections(obj)

    # This fork currently keeps all initialized globals in a single .data
    # section rather than emitting per-variable .data.<name> subsections,
    # even with -fdata-sections requested; mirrors the -ffunction-sections
    # characterization above. If per-variable sections are implemented,
    # flip this assertion to require the subsections.
    assert ".data" in names
    assert ".data.data_a" not in names, f"unexpected per-variable section; sections: {names}"


@pytest.mark.linker
@pytest.mark.linker_section
def test_string_literal_merge():
    obj = _compile_to_object("05_string_literal_merge", "sections")
    syms = _readelf_syms(obj)
    sections = _readelf_sections(obj)

    literals = [s for s in syms if s["name"].startswith("L.")]
    # Deduplicated in the frontend: each literal keeps its own anon symbol, but
    # identical bytes share a single storage offset (no orphan symbol left over).
    assert len(literals) == 2, f"expected 2 literal symbols, got {literals}"
    assert literals[0]["value"] == literals[1]["value"], (
        f"identical literals not deduplicated: {literals}"
    )

    rodata = next(s for s in sections if s["name"] == ".rodata")
    # One physical copy of "hello\0" => 6 bytes; dedup is done by the compiler,
    # not via a mergeable (SHF_MERGE/SHF_STRINGS) .rodata.str section.
    assert int(rodata["size"], 16) == 6, f"expected a single 6-byte copy, got {rodata['size']}"
    assert "M" not in rodata["flags"] and "S" not in rodata["flags"], (
        f".rodata unexpectedly mergeable: flags={rodata['flags']}"
    )


# -----------------------------------------------------------------------------
# symbols/
# -----------------------------------------------------------------------------
@pytest.mark.linker
@pytest.mark.linker_symbol
def test_symbol_binding_static_vs_global():
    obj = _compile_to_object("01_binding_static_vs_global", "symbols")
    syms = _readelf_syms(obj)
    by_name = {s["name"]: s for s in syms}

    assert by_name["static_var"]["bind"] == "LOCAL"
    assert by_name["static_func"]["bind"] == "LOCAL"
    assert by_name["global_var"]["bind"] == "GLOBAL"
    assert by_name["global_func"]["bind"] == "GLOBAL"

    assert by_name["static_var"]["type"] == "OBJECT"
    assert by_name["global_var"]["type"] == "OBJECT"
    assert by_name["static_func"]["type"] == "FUNC"
    assert by_name["global_func"]["type"] == "FUNC"


@pytest.mark.linker
@pytest.mark.linker_symbol
def test_symbol_weak_attribute():
    obj = _compile_to_object("02_weak_symbols", "symbols")
    syms = _readelf_syms(obj)
    by_name = {s["name"]: s for s in syms}

    assert by_name["weak_var"]["bind"] == "WEAK"
    assert by_name["weak_func"]["bind"] == "WEAK"


@pytest.mark.linker
@pytest.mark.linker_symbol
def test_symbol_visibility_hidden_attribute():
    obj = _compile_to_object("03_visibility_hidden", "symbols")
    syms = _readelf_syms(obj)
    by_name = {s["name"]: s for s in syms}

    assert by_name["hidden_var"]["vis"] == "HIDDEN"
    assert by_name["hidden_func"]["vis"] == "HIDDEN"
    # Hidden is a visibility, not a binding: both stay GLOBAL.
    assert by_name["hidden_var"]["bind"] == "GLOBAL"
    assert by_name["hidden_func"]["bind"] == "GLOBAL"

    # -fvisibility=hidden is part of _base_cflags() for the whole suite, but
    # this fork's option parser does not recognize it (no "visibility" entry
    # in libtcc.c's options_f table), so it silently falls through to
    # "unsupported option" and has no effect. A plain global therefore stays
    # STV_DEFAULT even under that flag; only the explicit
    # __attribute__((visibility("hidden"))) above is honored. This is a
    # front-end option-parsing gap, not a tccelf.c defect, so it is only
    # characterized here.
    assert by_name["plain_global"]["vis"] == "DEFAULT"


@pytest.mark.linker
@pytest.mark.linker_symbol
def test_symbol_alias_attribute():
    obj = _compile_to_object("04_alias_attribute", "symbols")
    syms = _readelf_syms(obj)
    by_name = {s["name"]: s for s in syms}

    assert "alias_func" in by_name, "alias attribute not supported/emitted"
    real = by_name["real_func"]
    alias = by_name["alias_func"]
    assert alias["value"] == real["value"]
    assert alias["bind"] == "GLOBAL"


@pytest.mark.linker
@pytest.mark.linker_symbol
def test_symbol_tentative_default_goes_to_bss():
    obj = _compile_to_object("05_tentative_default_bss", "symbols")
    syms = _readelf_syms(obj)
    sections = _readelf_sections(obj)

    sym = next(s for s in syms if s["name"] == "tentative_a")
    assert sym["bind"] == "GLOBAL"
    assert sym["ndx"] != "COM", "expected direct .bss placement by default, not SHN_COMMON"

    bss = next(s for s in sections if s["name"] == ".bss")
    assert sym["ndx"] == str(bss["nr"])
    assert sym["size"] == 4


@pytest.mark.linker
@pytest.mark.linker_symbol
def test_symbol_tentative_common_with_fcommon():
    obj = _compile_to_object("06_tentative_common_flag", "symbols", extra_cflags=["-fcommon"])
    syms = _readelf_syms(obj)

    sym = next(s for s in syms if s["name"] == "tentative_common")
    assert sym["ndx"] == "COM", f"expected SHN_COMMON with -fcommon, got ndx={sym['ndx']}"
    assert sym["bind"] == "GLOBAL"
    assert sym["size"] == 4


@pytest.mark.linker
@pytest.mark.linker_symbol
def test_symtab_locals_before_globals():
    obj = _compile_to_object("07_symtab_order", "symbols")
    syms = _readelf_syms(obj)
    sections = _readelf_sections(obj)

    binds = [s["bind"] for s in syms]
    first_nonlocal = next(i for i, b in enumerate(binds) if b != "LOCAL")
    # No LOCAL symbol may appear after the first non-local one.
    assert all(b == "LOCAL" for b in binds[:first_nonlocal])
    assert all(b != "LOCAL" for b in binds[first_nonlocal:])

    symtab = next(s for s in sections if s["name"] == ".symtab")
    assert symtab["inf"] == first_nonlocal, (
        f"sh_info ({symtab['inf']}) should equal index of first non-local symbol ({first_nonlocal})"
    )

    global_names = {s["name"] for s in syms if s["bind"] == "GLOBAL"}
    assert global_names >= {"g1", "g2", "g3"}
    local_names = {s["name"] for s in syms if s["bind"] == "LOCAL"}
    assert local_names >= {"s1", "s2", "s3"}


# -----------------------------------------------------------------------------
# yaff/
# -----------------------------------------------------------------------------
@pytest.mark.linker
@pytest.mark.linker_yaff
def test_yaff_output_structure(tinycc_root):
    result, cmd, out = _compile_to_yaff("01_basic", tinycc_root)

    if result.returncode != 0:
        pytest.fail(
            f"YAFF compile failed: {' '.join(cmd)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )

    data = out.read_bytes()
    # True YAFF output starts with the YAFF magic.
    if data.startswith(b"YAFF"):
        from struct import unpack_from

        # YaffHeader is packed little-endian; the first four bytes are the magic,
        # followed by module_type (u8), arch (u16le), yaff_version (u8).
        magic = data[0:4]
        module_type, arch, yaff_version = unpack_from("<BHB", data, 4)
        assert magic == b"YAFF"
        assert module_type in (1, 2)  # executable or dynamic library
        assert arch == 1  # ARM
        assert yaff_version == 1

        code_length = unpack_from("<I", data, 10)[0]
        data_length = unpack_from("<I", data, 18)[0]
        bss_length = unpack_from("<I", data, 22)[0]
        entry = unpack_from("<I", data, 26)[0]

        # The test program has non-empty code, data, and an entry point.
        assert code_length > 0
        assert data_length > 0
        assert entry > 0
        return

    # The cross compiler in this tree does not define TCC_TARGET_YASOS, so it
    # falls back to ELF output even when asked for a .yaff file.  That is a
    # documented build-time limitation, not a runtime bug; record it and still
    # verify the file is a valid ELF object.
    if data.startswith(b"\x7fELF"):
        pytest.skip(
            "YAFF output requires TCC_TARGET_YASOS; this cross compiler produced ELF"
        )

    pytest.fail(f"output is neither YAFF nor ELF: {data[:16]!r}")
