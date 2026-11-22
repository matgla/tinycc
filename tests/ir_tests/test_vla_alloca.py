"""-Wvla (a variable-length array declared) and -Walloca (a call to alloca or
__builtin_alloca): the stack a body takes at run time, which no frame limit
bounds -- and YasOS stacks are 4-8 KB.

Frontend checks at the declaration / the call, independent of -O; each is
reported once at its own line, also for a static inline function every call
inlined.  A VLA parameter takes no stack and is not reported (gcc's -Wvla
does report it).  Neither is in -Wall (nor in gcc's); both are in the safe
default, as warnings.  Quiet in a system header.
"""

import re
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

BODY = """extern int use(char *p);
static inline int iv(int n)
{
  char b[n];
  return use(b);
}
int a1(int n) { return iv(n) + iv(n + 1); }
int a2(int n)
{
  char *p = __builtin_alloca(n);
  int m[n][4];
  m[0][0] = 1;
  return use(p) + m[0][0];
}
void prm(int n, int a[n]) { a[0] = n; }
int fixed(void) { char b[16]; return use(b); }
"""


def _run(tmp_path, body, *flags, ccompat=True, extra=()):
    src = tmp_path / "t.c"
    src.write_text(body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / "t.o")]
    return subprocess.run(cmd + list(extra), capture_output=True, text=True)


def _hits(proc):
    return re.findall(r"t\.c:(\d+): (warning|error): (variable length array(?: '\w+')? is used|use of '\w+')",
                      proc.stderr)


EXPECTED = [("4", "variable length array 'b' is used"), ("10", "use of '__builtin_alloca'"),
            ("11", "variable length array 'm' is used")]


@pytest.mark.parametrize("opt", LEVELS + ["-fsyntax-only"])
def test_once_at_each_level(tmp_path, opt):
    proc = _run(tmp_path, BODY, opt, "-Wvla", "-Walloca")
    assert proc.returncode == 0, proc.stderr
    got = sorted((line, msg) for line, kind, msg in _hits(proc))
    assert got == sorted(EXPECTED), proc.stderr


def test_separately(tmp_path):
    assert [m for _, _, m in _hits(_run(tmp_path, BODY, "-Walloca"))] == ["use of '__builtin_alloca'"]
    assert sorted(m for _, _, m in _hits(_run(tmp_path, BODY, "-Wvla"))) == [
        "variable length array 'b' is used", "variable length array 'm' is used"]


def test_plain_alloca(tmp_path):
    body = "void *alloca(unsigned);\nextern int use(char *p);\nint f(int n) { char *p = alloca(n); return use(p); }\n"
    assert [m for _, _, m in _hits(_run(tmp_path, body, "-Walloca"))] == ["use of 'alloca'"]


def test_off_by_default_in_ccompat(tmp_path):
    assert _hits(_run(tmp_path, BODY)) == []
    assert _hits(_run(tmp_path, BODY, "-Wall")) == []
    assert _hits(_run(tmp_path, BODY, "-Wvla", "-Walloca", "-Wno-vla", "-Wno-alloca")) == []
    assert _hits(_run(tmp_path, BODY, "-Wvla", "-Walloca", "-w")) == []


def test_safe_default(tmp_path):
    body = BODY.replace("char *p", "char * view p").replace("char *p =", "char * view p =")
    body = body.replace("char * view p = __builtin_alloca(n)", "char * view p = (char * view)__builtin_alloca(n)")
    proc = _run(tmp_path, body, ccompat=False)
    assert proc.returncode == 0, proc.stderr
    assert sorted((l, m) for l, k, m in _hits(proc)) == sorted(EXPECTED), proc.stderr
    assert all(k == "warning" for _, k, _ in _hits(proc))
    assert _hits(_run(tmp_path, body, "-Wno-vla", "-Wno-alloca", ccompat=False)) == []


def test_errors(tmp_path):
    proc = _run(tmp_path, BODY, "-Werror=vla")
    assert proc.returncode != 0 and {k for _, k, _ in _hits(proc)} == {"error"}
    proc = _run(tmp_path, BODY, "-Wvla", "-Walloca", "-Werror", "-Wno-error=vla", "-Wno-error=alloca")
    assert proc.returncode == 0 and len(_hits(proc)) == 3, proc.stderr
    proc = _run(tmp_path, BODY, "-Wvla", "-Walloca", "-Werror")
    assert proc.returncode != 0


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_pragma(tmp_path, opt):
    body = ('#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wvla"\n'
            '#pragma GCC diagnostic ignored "-Walloca"\n' + BODY + "#pragma GCC diagnostic pop\n"
            "int later(int n) { char c[n]; return use(c); }\n")
    proc = _run(tmp_path, body, opt, "-Wvla", "-Walloca")
    assert [(l, m) for l, _, m in _hits(proc)] == [("21", "variable length array 'c' is used")], proc.stderr


def test_system_header_quiet(tmp_path):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "s.h").write_text("extern int use(char *p);\n"
                              "static int sv(int n) { char b[n]; return use(b) + *(char *)__builtin_alloca(1); }\n")
    proc = _run(tmp_path, '#include <s.h>\nint f(int n) { return sv(n); }\n', "-O0", "-Wvla", "-Walloca",
                extra=["-isystem", str(inc)])
    assert _hits(proc) == [] and "warning" not in proc.stderr, proc.stderr


def test_objects_unchanged(tmp_path):
    for opt in LEVELS:
        _run(tmp_path, BODY, opt)
        a = (tmp_path / "t.o").read_bytes()
        _run(tmp_path, BODY, opt, "-Wvla", "-Walloca")
        assert (tmp_path / "t.o").read_bytes() == a, opt
