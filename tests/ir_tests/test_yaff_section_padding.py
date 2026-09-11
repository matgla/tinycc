"""The YAFF writer and sections whose sh_size outgrows their contents.

Layout rounds a section's sh_size up to the segment alignment when the section
opens a new segment, and without -section-alignment that alignment is the page
size -- so a program with 256 bytes of .data gets a 4096-byte .data.  The writer
used to fwrite all sh_size bytes straight out of the section buffer, reading
3840 bytes past its end: ASan aborted the link before the header was rewritten.

The rootfs always links with -section-alignment=0x4, which keeps the rounding
to a few bytes and hid this.  These links deliberately do not.
"""

import struct
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC_TOP = CURRENT_DIR.parent.parent
TCC = TCC_TOP / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

# YaffHeader (source/obj/tccyaff.h) is packed; offsets of the fields read here.
CODE_LENGTH = 8
DATA_LENGTH = 16
GOT_LENGTH = 48
PLT_LENGTH = 56
ARCH_SECTION_OFFSET = 60
TEXT_OFFSET = 70

SOURCES = {
    # Small .data, far below a page: the case that overflowed.
    "small_data": """
int counter = 5;
int table[37] = {1, 2, 3};
void _start(void) { table[1] = counter; }
""",
    # .data larger than the old 256-byte buffer, still below a page.
    "larger_data": """
char big[600] = {1};
int x = 3;
void _start(void) { big[1] = x; }
""",
}


@pytest.mark.parametrize("name", sorted(SOURCES))
def test_default_page_layout_links_and_matches_its_header(tmp_path, name):
    src = tmp_path / f"{name}.c"
    src.write_text(SOURCES[name])
    out = tmp_path / f"{name}.yaff"
    proc = subprocess.run(
        [
            str(TCC),
            f"-B{TCC_TOP}",
            f"-L{TCC_TOP}/lib",
            f"-L{TCC_TOP}/lib/fp",
            "-nostdlib",
            "-fpie",
            "-fPIE",
            "-Wl,-oformat=yaff",
            "-o",
            str(out),
            str(src),
        ],
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0, f"link failed:\n{proc.stdout}{proc.stderr}"

    data = out.read_bytes()
    assert data[:4] == b"YAFF"
    # Rewritten at the very end of tcc_output_yaff; still 0 if the writer
    # stopped before it.
    assert struct.unpack_from("<H", data, ARCH_SECTION_OFFSET)[0] != 0

    # text, plt, rodata+data (data_length) and got are the last things written,
    # back to back, so the file ends exactly where the header says the image
    # does.  A writer that emitted the padded sizes short, or long, fails here.
    (text_offset,) = struct.unpack_from("<H", data, TEXT_OFFSET)
    (code_length,) = struct.unpack_from("<I", data, CODE_LENGTH)
    (data_length,) = struct.unpack_from("<I", data, DATA_LENGTH)
    (got_length,) = struct.unpack_from("<I", data, GOT_LENGTH)
    (plt_length,) = struct.unpack_from("<I", data, PLT_LENGTH)
    assert len(data) == text_offset + code_length + plt_length + data_length + got_length
