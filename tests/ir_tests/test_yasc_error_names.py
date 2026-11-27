"""yasc error names: the yasc_errname records and the link-time checks.

Every yasc file records the error names it uses ({u16 value, u8 flags, u8
len, name}); the linker reads all of them: one name with two values, or two
names with one value where a value came from a hash, is an error.  Explicit
aliases (`FileNotFound = ENOENT`) are fine.  error_name() is in
yasc_error_name.c (test_qemu.py).  docs/plans/yasc_errors.md.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
ROOT = CURRENT_DIR.parent.parent
TCC = ROOT / "armv8m-tcc"
LIBS = [f"-B{ROOT}", f"-L{ROOT}/lib", f"-L{ROOT}/lib/fp"]
READELF = shutil.which("arm-none-eabi-readelf")

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

START = "void _start(void) { for (;;) ; }\n"


def _link(tmp_path, *sources):
    files = []
    for i, body in enumerate(sources):
        f = tmp_path / f"f{i}.c"
        f.write_text(body)
        files.append(str(f))
    return subprocess.run(
        [str(TCC), "-ccompat", "-nostdlib", *LIBS, *files, "-o", str(tmp_path / "out.elf")],
        capture_output=True,
        text=True,
    )


def test_one_name_two_values(tmp_path):
    a = "#pragma yasc\nerror S { Zork = 0x2001 };\n!int fa(void) { return error.Zork; }\n" + START
    b = "#pragma yasc\n!int fb(void) { return error.Zork; }\n"
    proc = _link(tmp_path, a, b)
    assert proc.returncode != 0
    assert "error.Zork has two values" in proc.stderr, proc.stderr


def test_two_hashed_names_one_value(tmp_path):
    a = "#pragma yasc\n!int fa(void) { return error.ErrAUY; }\n" + START
    b = "#pragma yasc\n!int fb(void) { return error.ErrDAD; }\n"
    proc = _link(tmp_path, a, b)
    assert proc.returncode != 0
    assert "have the same value 21901" in proc.stderr, proc.stderr


def test_hash_collision_in_one_file(tmp_path):
    a = "#pragma yasc\n!int fa(int x) { if (x) return error.ErrAUY; return error.ErrDAD; }\n" + START
    proc = _link(tmp_path, a)
    assert proc.returncode != 0
    assert "have the same value 21901" in proc.stderr, proc.stderr


def test_explicit_alias_links(tmp_path):
    a = "#pragma yasc\n#define ENOENT 2\nerror S { FileNotFound = ENOENT };\n"
    a += "!int fa(int x) { if (x) return error.FileNotFound; return error.ENOENT; }\n" + START
    b = "#pragma yasc\n#define ENOENT 2\n!int fb(void) { return error.ENOENT; }\n"
    proc = _link(tmp_path, a, b)
    assert proc.returncode == 0, proc.stderr


def test_same_hashed_name_in_two_files_links(tmp_path):
    a = "#pragma yasc\n!int fa(void) { return error.Shared; }\n" + START
    b = "#pragma yasc\n!int fb(void) { return error.Shared; }\n"
    proc = _link(tmp_path, a, b)
    assert proc.returncode == 0, proc.stderr


@pytest.mark.skipif(not READELF, reason="needs arm-none-eabi-readelf")
def test_records_only_in_yasc_files(tmp_path):
    yasc = tmp_path / "y.c"
    yasc.write_text("#pragma yasc\n!int f(void) { return error.Named; }\n")
    plain = tmp_path / "p.c"
    plain.write_text("int f(void) { return 1; }\n")
    for src in (yasc, plain):
        obj = src.with_suffix(".o")
        proc = subprocess.run([str(TCC), "-ccompat", "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
        assert proc.returncode == 0, proc.stderr
    sections = lambda o: subprocess.run([READELF, "-S", str(o)], capture_output=True, text=True).stdout
    assert "yasc_errname" in sections(tmp_path / "y.o")
    assert "yasc_errname" not in sections(tmp_path / "p.o")


def test_error_name_outside_yasc_is_an_identifier(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("int error_name = 3;\nint f(void) { return error_name; }\n")
    proc = subprocess.run([str(TCC), "-ccompat", "-c", str(src), "-o", str(tmp_path / "t.o")], capture_output=True, text=True)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


@pytest.mark.skipif(READELF is None, reason="needs arm-none-eabi-readelf")
def test_error_name_reads_only_errno_macros(tmp_path):
    # error_name() records the E[A-Z0-9]+ macros: it must not evaluate the
    # others (UINT64_MAX-like constants warned "integer constant overflow")
    src = tmp_path / "t.c"
    src.write_text(
        "#pragma yasc\n#define TOO_BIG 0x1ffffffffffffffff\n#define SMALL 7\n#define EZORK 77\n"
        "const char *?borrows(static) g(int e)\n{\n  return error_name(e);\n}\n"
    )
    obj = tmp_path / "t.o"
    proc = subprocess.run([str(TCC), "-ccompat", "-Wall", "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    data = subprocess.run([READELF, "-x", "yasc_errname", str(obj)], capture_output=True, text=True).stdout
    assert "EZORK" in data and "SMALL" not in data, data
