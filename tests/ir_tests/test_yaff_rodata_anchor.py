"""The YAFF writer and the rodata anchor (GOT index 3).

Under -share-rodata the code reaches .rodata through [r9, #24], a GOT slot the
loader fills from a local relocation.  The writer emitted that relocation after
the loop over .rel.got and returned before it when the GOT had no relocations
-- a module with .rodata but no imports, like libsoftfp.so once divc3.c gave
it constants.  Its slot stayed 0: __divsc3 faulted on QEMU and __divdc3 read
the board's ROM as its constants (912_complex_div_runtime on the device).
"""

import struct
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC_TOP = CURRENT_DIR.parent.parent
TCC = TCC_TOP / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

# YaffHeader (source/obj/tccyaff.h), packed
SYMBOL_TABLE_RELOCATIONS_AMOUNT = 36
LOCAL_RELOCATIONS_AMOUNT = 40
RELOCATIONS_OFFSET = 80
SYMBOL_TABLE_RELOCATION_SIZE = 8

YAFF_SECTION_DATA = 1
ANCHOR_INDEX = 3

# Two .rodata reads in one function and one in another: the prologue-held
# anchor and the push/pop form.
RODATA = """
static const double K[4] = {1.5, 2.5, 3.5, 4.5};
static const float F[2] = {0.25f, 0.75f};
double pick(int i) { return K[i & 3] + K[(i + 1) & 3]; }
float pickf(int i) { return F[i & 1]; }
"""

# The same, reading another module's variable: the GOT has relocations.
RODATA_IMPORT = RODATA + """
extern int other;
int read_other(int i) { return other + (int)F[i & 1]; }
"""


def _link_so(tmp_path, name, source, *extra):
    src = tmp_path / f"{name}.c"
    src.write_text(source)
    out = tmp_path / f"lib{name}.so"
    proc = subprocess.run(
        [
            str(TCC),
            "-ccompat",
            f"-B{TCC_TOP}",
            f"-L{TCC_TOP}/lib",
            "-shared",
            "-fPIC",
            "-nostdlib",
            "-mfloat-abi=soft",
            "-Wl,-section-alignment=0x4",
            "-Wl,-oformat=yaff",
            *extra,
            "-o",
            str(out),
            str(src),
        ],
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0, f"link failed:\n{proc.stdout}{proc.stderr}"
    return out.read_bytes()


def _local_relocations(data):
    assert data[:4] == b"YAFF"
    symtab = struct.unpack_from("<I", data, SYMBOL_TABLE_RELOCATIONS_AMOUNT)[0]
    count = struct.unpack_from("<I", data, LOCAL_RELOCATIONS_AMOUNT)[0]
    start = struct.unpack_from("<I", data, RELOCATIONS_OFFSET)[0] + symtab * SYMBOL_TABLE_RELOCATION_SIZE
    relocs = []
    for k in range(count):
        word, offset = struct.unpack_from("<II", data, start + 8 * k)
        relocs.append((word & 3, word >> 2, offset))
    return relocs


def test_anchor_without_got_relocations(tmp_path):
    relocs = _local_relocations(_link_so(tmp_path, "noimport", RODATA))
    assert (YAFF_SECTION_DATA, ANCHOR_INDEX, 0) in relocs


def test_anchor_with_got_relocations(tmp_path):
    relocs = _local_relocations(_link_so(tmp_path, "import", RODATA_IMPORT))
    assert (YAFF_SECTION_DATA, ANCHOR_INDEX, 0) in relocs


def test_no_anchor_without_share_rodata(tmp_path):
    relocs = _local_relocations(_link_so(tmp_path, "noshare", RODATA, "-no-share-rodata"))
    assert all(index != ANCHOR_INDEX for _, index, _ in relocs)
