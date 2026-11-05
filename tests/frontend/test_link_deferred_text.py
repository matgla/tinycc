"""Linking: a .text.<fn> section's bytes stay on disk until they fold into .text.

A link of tcc itself needs 11.5 MB if every function is held twice, and a device
has 8.7.  The deferral must be invisible in the output, wherever the bytes live.
"""

import struct
import subprocess
from pathlib import Path

import pytest

from conftest import _find_compiler

LIVE = "int live_a(int x) { return x * 3 + 1; }\nint live_b(int x) { return x ^ 0x5a; }\n"
DEAD = "int dead_c(int x) { return x * 7 - 2; }\n"
MAIN = "extern int live_a(int), live_b(int);\nint _start(void) { return live_a(1) + live_b(2); }\n"


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


def sections(data):
    """{name: (header offset, sh_offset, sh_size, sh_addr)} of an ELF32 image."""
    shoff = struct.unpack_from("<I", data, 32)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 46)
    def hdr(i):
        return struct.unpack_from("<10I", data, shoff + i * shentsize)
    strs = hdr(shstrndx)[4]
    out = {}
    for i in range(shnum):
        h = hdr(i)
        end = data.index(b"\0", strs + h[0])
        out[data[strs + h[0]:end].decode()] = (shoff + i * shentsize, h[4], h[5], h[3])
    return out


def section_bytes(data, name):
    _, off, size, _ = sections(data)[name]
    return data[off:off + size]


def build(tcc, tmp_path, source, name):
    src, obj = tmp_path / f"{name}.c", tmp_path / f"{name}.o"
    src.write_text(source)
    r = subprocess.run([tcc, "-O1", "-ffunction-sections", "-nostdlib", "-c", str(src), "-o", str(obj)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return obj


def link(tcc, tmp_path, *inputs):
    out = tmp_path / "out.elf"
    root = Path(tcc).parent
    return subprocess.run([tcc, f"-B{root}", f"-L{root}", f"-L{root}/lib", "-nostdlib", "-static",
                           "-Wl,--gc-sections", "-Wl,-oformat=elf32-littlearm", "-Wl,-e,_start",
                           *map(str, inputs), "-o", str(out)],
                          capture_output=True, text=True), out


def assert_live_code_linked(tcc, tmp_path, lib_obj, *inputs):
    r, out = link(tcc, tmp_path, *inputs)
    assert r.returncode == 0, r.stderr
    text = section_bytes(out.read_bytes(), ".text")
    obj = lib_obj.read_bytes()
    assert section_bytes(obj, ".text.live_a") in text
    assert section_bytes(obj, ".text.live_b") in text
    assert section_bytes(obj, ".text.dead_c") not in text


def test_object_file_functions_reach_text(tcc, tmp_path):
    lib = build(tcc, tmp_path, LIVE + DEAD, "lib")
    assert_live_code_linked(tcc, tmp_path, lib, build(tcc, tmp_path, MAIN, "main"), lib)


def test_archive_member_functions_reach_text(tcc, tmp_path):
    """A member sits at a non-zero offset in its archive, and not the first one."""
    pad = build(tcc, tmp_path, "int pad_fn(int x) { return x + 11; }\n", "pad")
    lib = build(tcc, tmp_path, LIVE + DEAD, "lib")
    ar = tmp_path / "liblib.a"
    r = subprocess.run([tcc, "-ar", "rcs", str(ar), str(pad), str(lib)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert_live_code_linked(tcc, tmp_path, lib, build(tcc, tmp_path, MAIN, "main"), ar)


def move_section_past_eof(path, name):
    data = bytearray(path.read_bytes())
    header = sections(bytes(data))[name][0]
    struct.pack_into("<I", data, header + 16, len(data) + 0x1000)
    path.write_bytes(bytes(data))


def test_a_function_that_cannot_be_read_fails_the_link(tcc, tmp_path):
    """It must not link with zeroes where the code should be."""
    lib = build(tcc, tmp_path, LIVE + DEAD, "lib")
    main = build(tcc, tmp_path, MAIN, "main")
    move_section_past_eof(lib, ".text.live_a")
    r, _ = link(tcc, tmp_path, main, lib)
    assert r.returncode != 0
    assert "short read" in r.stderr


def test_a_collected_function_is_never_read(tcc, tmp_path):
    lib = build(tcc, tmp_path, LIVE + DEAD, "lib")
    main = build(tcc, tmp_path, MAIN, "main")
    move_section_past_eof(lib, ".text.dead_c")
    r, _ = link(tcc, tmp_path, main, lib)
    assert r.returncode == 0, r.stderr
