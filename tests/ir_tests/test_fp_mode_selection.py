"""-mfp-inline and -mfp-lib: the three ways to reach a floating point operation.

A double on RP2350 can be computed three ways, and they differ in *where the
arithmetic lives*: in libsoftfp's C, in librp2350fp's DCP sequences one call
away, or in the caller's own instruction stream.  Three properties have to hold
for a build to actually get the one it asked for, and none of them is visible
from the source:

  1. whether any FP instruction is emitted inline at all,
  2. which __aeabi_ runtime the link binds, and
  3. what the YAFF architecture section then declares the image needs.

They used to disagree.  The middle mode had no flag of its own and could only
be spelled `-mfloat-abi=soft -mfpu=rp2350`, which meant "emit no FP
instructions" and yet linked a runtime full of DCP ones -- while writing a
header that asked the loader for nothing, so the image would have been accepted
on a part with no DCP and faulted on the first double.  This pins all three down
per mode.

The header is read with scripts/readyaff.c's rules rather than by shelling out
to it: the field offsets come from tccyaff.h and are asserted below, so a layout
change breaks this test loudly instead of silently reading the wrong byte.
"""

import shutil
import struct
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC_TOP = CURRENT_DIR.parent.parent
TCC = TCC_TOP / "armv8m-tcc"
OBJDUMP = shutil.which("arm-none-eabi-objdump")

pytestmark = pytest.mark.skipif(
    OBJDUMP is None or not TCC.exists(),
    reason="needs armv8m-tcc and arm-none-eabi-objdump",
)

SOURCE = """
double dadd(double a, double b) { return a + b; }
double dmul(double a, double b) { return a * b; }
float  fadd(float a, float b)   { return a + b; }
void _start(void) { }
"""

# The DCP encodings (cdp/stcl/ldcl on coprocessor 4) and the FPv5-SP ones are
# the only instructions that can appear when an operation is lowered inline; a
# lowered-to-a-call arm emits none of them.
INLINE_FP_MNEMONICS = ("cdp", "stcl", "ldcl", "vadd", "vsub", "vmul", "vdiv")


def _compile(tmp_path, name, *flags):
    src = tmp_path / f"{name}.c"
    src.write_text(SOURCE)
    obj = tmp_path / f"{name}.o"
    proc = subprocess.run(
        [str(TCC), f"-B{TCC_TOP}", *flags, "-c", str(src), "-o", str(obj)],
        capture_output=True,
        text=True,
    )
    assert obj.exists(), f"compile failed: {proc.stdout}{proc.stderr}"
    return obj


def _inline_fp_instruction_count(obj):
    listing = subprocess.run(
        [OBJDUMP, "-d", str(obj)], capture_output=True, text=True, check=True
    ).stdout
    return sum(
        1
        for line in listing.splitlines()
        if any(f"\t{m}" in line for m in INLINE_FP_MNEMONICS)
    )


def _link_verbose(tmp_path, name, *flags):
    """Link and return the FP-library line tcc prints under -v."""
    src = tmp_path / f"{name}.c"
    src.write_text(SOURCE)
    out = tmp_path / name
    proc = subprocess.run(
        [
            str(TCC),
            f"-B{TCC_TOP}",
            f"-L{TCC_TOP}/lib",
            f"-L{TCC_TOP}/lib/fp",
            "-nostdlib",
            "-v",
            *flags,
            "-o",
            str(out),
            str(src),
        ],
        capture_output=True,
        text=True,
    )
    combined = proc.stdout + proc.stderr
    for line in combined.splitlines():
        if "ARM FP" in line:
            return line.strip(), out
    raise AssertionError(f"no FP library line in:\n{combined}")


def test_soft_fpu_inlines_nothing_and_binds_the_software_runtime(tmp_path):
    obj = _compile(tmp_path, "soft", "-mfpu=none")
    assert _inline_fp_instruction_count(obj) == 0
    line, _ = _link_verbose(tmp_path, "soft", "-mfpu=none")
    assert "libsoftfp" in line


def test_default_fpu_inlines_what_the_backend_implements(tmp_path):
    """The control for the two tests below: with no restriction, something is
    inlined.  If this ever reports zero the other assertions become vacuous."""
    obj = _compile(tmp_path, "inline", "-mfpu=rp2350")
    assert _inline_fp_instruction_count(obj) > 0


def test_fp_inline_none_keeps_the_hardware_runtime(tmp_path):
    """The mode that had no name: no inline FP, hardware runtime anyway.

    This is what separates it from -mfloat-abi=soft, which means the software
    runtime as well as no instructions.
    """
    obj = _compile(tmp_path, "hwlib", "-mfpu=rp2350", "-mfp-inline=none")
    assert _inline_fp_instruction_count(obj) == 0
    line, _ = _link_verbose(tmp_path, "hwlib", "-mfpu=rp2350", "-mfp-inline=none")
    assert "librp2350fp" in line


def test_float_abi_soft_binds_the_software_runtime(tmp_path):
    """-mfloat-abi=soft means no FP instructions anywhere, and the runtime an
    image links is part of the image.  Pairing it with a hardware -mfpu used to
    link librp2350fp -- DCP instructions in a build that had just declared it
    emits none."""
    line, _ = _link_verbose(tmp_path, "abisoft", "-mfpu=rp2350", "-mfloat-abi=soft")
    assert "libsoftfp" in line


def test_fp_lib_shared_binds_a_shared_object(tmp_path):
    line, _ = _link_verbose(tmp_path, "shared", "-mfpu=rp2350", "-mfp-lib=shared")
    assert "shared library" in line and "librp2350fp" in line


def test_fp_lib_static_binds_an_archive(tmp_path):
    line, _ = _link_verbose(tmp_path, "static", "-mfpu=rp2350", "-mfp-lib=static")
    assert "fp/librp2350fp.a" in line


# --- the YAFF architecture section -------------------------------------------
#
# Layout from source/obj/tccyaff.h.  YaffHeader is packed, so the one field this
# needs -- arch_section_offset -- is at a fixed byte offset, and following it is
# better than recomputing the section's position from the module name's padded
# length the way tcc_get_offset_to_imported_libraries() has to: the header says
# where it put the section, and a writer that moves it moves this too.

# Byte offset of YaffHeader.arch_section_offset, counted through the packed
# fields ahead of it.  Cross-checked by test_header_layout_is_still_what_this
# _test_assumes below, which reads a value no misaligned read would produce.
ARCH_SECTION_OFFSET_FIELD = 60

# YaffArchSection: u16 size, u8 arch, u8 fpu, u8 float_abi, u8 reserved_[3],
# u32 required_features.
ARCH_SECTION_FORMAT = "<HBBB3xI"

YAFF_ARCH_FEATURE_FPU_SP = 1 << 0
YAFF_ARCH_FEATURE_FPU_DP = 1 << 1
YAFF_ARCH_FEATURE_DCP = 1 << 2

YAFF_FPU_NONE = 0
YAFF_FPU_RP2350 = 4

YAFF_FLOAT_ABI_SOFT = 0
YAFF_FLOAT_ABI_SOFTFP = 1


def _yaff_arch_section(path):
    data = path.read_bytes()
    assert data[:4] == b"YAFF", "not a YAFF module"
    offset = struct.unpack_from("<H", data, ARCH_SECTION_OFFSET_FIELD)[0]
    assert offset != 0, "no architecture section (YAFF version 1 image?)"
    size, _arch, fpu, float_abi, required = struct.unpack_from(
        ARCH_SECTION_FORMAT, data, offset
    )
    assert size == struct.calcsize(ARCH_SECTION_FORMAT), (
        f"architecture section is {size} bytes, this test reads "
        f"{struct.calcsize(ARCH_SECTION_FORMAT)}"
    )
    return {"fpu": fpu, "float_abi": float_abi, "required_features": required}


def _link_yaff(tmp_path, name, *flags):
    src = tmp_path / f"{name}.c"
    src.write_text(SOURCE)
    out = tmp_path / f"{name}.yaff"
    proc = subprocess.run(
        [
            str(TCC),
            f"-B{TCC_TOP}",
            f"-L{TCC_TOP}/lib",
            f"-L{TCC_TOP}/lib/fp",
            "-nostdlib",
            # Link the way every rootfs Makefile links a YAFF image.  YAFF is only
            # the default output of a TCC_TARGET_YASOS build -- the plain
            # `--enable-cross` compiler CI builds writes ELF unless asked -- and
            # without -Ttext/-section-alignment the writer falls over the ELF
            # page layout (ASan: heap overflow writing .data).
            "-Wl,-oformat=yaff",
            "-Wl,-Ttext=0x0",
            "-Wl,-section-alignment=0x4",
            *flags,
            "-o",
            str(out),
            str(src),
        ],
        capture_output=True,
        text=True,
    )
    assert out.exists(), f"link failed: {proc.stdout}{proc.stderr}"
    return _yaff_arch_section(out)


def test_header_layout_is_still_what_this_test_assumes(tmp_path):
    """A guard on the reader above.  An image built for no FP unit has to come
    out naming no FPU, a soft ABI and no required features -- three fields whose
    values a misaligned read would not produce together."""
    arch = _link_yaff(tmp_path, "layout", "-mfpu=none")
    assert arch == {
        "fpu": YAFF_FPU_NONE,
        "float_abi": YAFF_FLOAT_ABI_SOFTFP,
        "required_features": 0,
    }


def test_inline_hardware_declares_the_dcp(tmp_path):
    arch = _link_yaff(tmp_path, "arch_inline", "-mfpu=rp2350")
    assert arch["fpu"] == YAFF_FPU_RP2350
    assert arch["required_features"] == YAFF_ARCH_FEATURE_FPU_SP | YAFF_ARCH_FEATURE_DCP


def test_fp_inline_none_still_declares_the_dcp(tmp_path):
    """The image emits no DCP instruction of its own, but the runtime it binds
    is made of them -- statically it carries them, dynamically it depends on a
    library that does.  Either way the process needs a part with a DCP."""
    arch = _link_yaff(tmp_path, "arch_hwlib", "-mfpu=rp2350", "-mfp-inline=none")
    assert arch["fpu"] == YAFF_FPU_RP2350
    assert arch["required_features"] == YAFF_ARCH_FEATURE_FPU_SP | YAFF_ARCH_FEATURE_DCP


def test_float_abi_soft_declares_nothing(tmp_path):
    arch = _link_yaff(tmp_path, "arch_soft", "-mfpu=rp2350", "-mfloat-abi=soft")
    assert arch["fpu"] == YAFF_FPU_NONE
    assert arch["float_abi"] == YAFF_FLOAT_ABI_SOFT
    assert arch["required_features"] == 0
