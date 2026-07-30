"""ARM EABI build attributes (.ARM.attributes) and ELF float-ABI flags.

The attribute section is what lets a linker check ABI compatibility between
objects.  The float trio is the part that matters most:

    Tag_FP_arch         which FP unit's instructions may appear
    Tag_ABI_HardFP_use  "SP only" when the unit has no double precision
    Tag_ABI_VFP_args    present only for -mfloat-abi=hard; its absence means
                        the base (GPR) argument standard

These are emitted at link time by create_arm_attribute_section() (tccelf.c) from
values the ARM backend derives in arm_get_eabi_attrs().  The section used to be a
hardcoded ARMv6 blob patched at fixed byte offsets, which silently misdescribed
every ARMv8-M binary — hence this test.
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"
READELF = shutil.which("arm-none-eabi-readelf")

pytestmark = pytest.mark.skipif(
    READELF is None or not TCC.exists(),
    reason="needs armv8m-tcc and arm-none-eabi-readelf",
)

SOURCE = """
float addf(float a, float b) { return a + b; }
void _start(void) { }
"""


def _link(tmp_path, *extra_flags):
    src = tmp_path / "attr.c"
    src.write_text(SOURCE)
    elf = tmp_path / "attr.elf"
    top = TCC.parent
    cmd = [
        str(TCC),
        f"-B{top}",
        # soft/softfp pull in the __aeabi_* FP runtime unconditionally.
        f"-L{top}/lib",
        f"-L{top}/lib/fp",
        f"-L{top}",
        "-nostdlib",
        "-Wl,-oformat=elf32-littlearm",
        *extra_flags,
        "-o",
        str(elf),
        str(src),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    assert elf.exists(), f"link failed: {proc.stdout}{proc.stderr}"
    return elf


def _attributes(elf):
    """Parse `readelf -A` into {tag: value}."""
    out = subprocess.run([READELF, "-A", str(elf)], capture_output=True, text=True).stdout
    attrs = {}
    for line in out.splitlines():
        m = re.match(r"\s+(Tag_\w+):\s*(.*)", line)
        if m:
            attrs[m.group(1)] = m.group(2).strip()
    assert attrs, f"no ARM attributes found:\n{out}"
    return attrs


def _elf_flags(elf):
    out = subprocess.run([READELF, "-h", str(elf)], capture_output=True, text=True).stdout
    m = re.search(r"Flags:\s+\S+,\s*(.*)", out)
    assert m, f"no ELF flags found:\n{out}"
    return m.group(1)


@pytest.mark.parametrize("abi", ["soft", "softfp", "hard"])
def test_float_abi_attributes(abi, tmp_path):
    """Each float ABI must declare itself correctly.

    soft   : no FP unit advertised at all (no FP instructions are emitted)
    softfp : FP unit advertised, but arguments still travel in GPRs
    hard   : FP unit advertised and arguments travel in VFP registers
    """
    elf = _link(tmp_path, f"-mfloat-abi={abi}", "-mfpu=fpv5-sp-d16")
    attrs = _attributes(elf)

    if abi == "soft":
        assert "Tag_FP_arch" not in attrs
        assert "Tag_ABI_HardFP_use" not in attrs
    else:
        assert attrs["Tag_FP_arch"] == "FPv5/FP-D16 for ARMv8"
        assert attrs["Tag_ABI_HardFP_use"] == "SP only"

    # The attribute that makes a hard-float object incompatible with a
    # soft-float one.  Absent => base standard (GPR) argument passing.
    if abi == "hard":
        assert attrs["Tag_ABI_VFP_args"] == "VFP registers"
    else:
        assert "Tag_ABI_VFP_args" not in attrs

    # e_flags must agree with the attributes: only hard-float passes FP
    # arguments in VFP registers, so only it is marked hard-float ABI.
    expected_flag = "hard-float ABI" if abi == "hard" else "soft-float ABI"
    assert expected_flag in _elf_flags(elf)


def test_cpu_attributes_describe_armv8m(tmp_path):
    """The CPU attributes must describe ARMv8-M, not the old hardcoded ARMv6.

    Tag_ARM_ISA_use in particular must be absent: M-profile is Thumb-only.
    """
    attrs = _attributes(_link(tmp_path, "-mfpu=fpv5-sp-d16"))
    assert attrs["Tag_CPU_name"] == '"8-M.MAIN"'
    assert attrs["Tag_CPU_arch"] == "v8-M.mainline"
    assert attrs["Tag_CPU_arch_profile"] == "Microcontroller"
    assert attrs["Tag_THUMB_ISA_use"] == "Yes"
    assert "Tag_ARM_ISA_use" not in attrs


def test_no_fpu_advertises_no_fp_arch(tmp_path):
    """-mfpu=none must not advertise an FP unit."""
    attrs = _attributes(_link(tmp_path, "-mfpu=none", "-mfloat-abi=softfp"))
    assert "Tag_FP_arch" not in attrs
    assert "Tag_ABI_VFP_args" not in attrs
