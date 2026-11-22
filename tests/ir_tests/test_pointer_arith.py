"""-Wpointer-arith: what depends on the size of void or of a function type,
which GNU C takes as 1 -- arithmetic on a `void *` or a function pointer
(+, -, ++, --, +=, a difference of two), `sizeof(void)`, `sizeof` of a
function, `_Alignof(void)`.

gcc has it in -Wpedantic only; so does tcc, outside -Wall and the safe
default: `void *` arithmetic is common GNU C (45 reports over our C).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _run(tmp_path, body, *flags, compat=True, out="t.o"):
    src = tmp_path / "t.c"
    src.write_text(body)
    cmd = [str(TCC)] + (["-ccompat"] if compat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / out)]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc, kind="warning"):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] == kind:
            out.append((int(parts[0].rsplit(":", 1)[1]), parts[2]))
    return out


ARITH = "pointer of type 'void *' used in arithmetic"
SUB = "pointer of type 'void *' used in subtraction"

BODY = """\
int f(void *vp, void *vq, char *cp, int (*fp)(void)) {
  int x = vp - vq;
  vp++;
  vp += 2;
  x += sizeof(void);
  x += sizeof(f);
  x += __alignof__(void);
  fp = fp + 1;
  x += cp - (char *)vq;
  cp += 3;
  x += vp == vq;
  x += sizeof(void *) + sizeof(fp);
  return x + (fp != 0) + (cp != 0);
}
"""

EXPECTED = [
    (2, SUB),
    (3, ARITH),
    (4, ARITH),
    (5, "invalid application of 'sizeof' to a void type"),
    (6, "invalid application of 'sizeof' to a function type"),
    (7, "invalid application of '__alignof__' to a void type"),
    (8, "pointer to a function used in arithmetic"),
]


@pytest.mark.parametrize("opt", LEVELS)
def test_hits(tmp_path, opt):
    proc = _run(tmp_path, BODY, "-Wpointer-arith", opt)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == EXPECTED, proc.stderr


def test_switches(tmp_path):
    assert _diags(_run(tmp_path, BODY)) == []
    assert _diags(_run(tmp_path, BODY, "-Wall")) == []
    assert _diags(_run(tmp_path, BODY, "-Wpointer-arith", "-Wno-pointer-arith")) == []


def test_werror(tmp_path):
    proc = _run(tmp_path, BODY, "-Werror=pointer-arith")
    assert proc.returncode != 0 and f"error: {SUB}" in proc.stderr, proc.stderr
    proc = _run(tmp_path, BODY, "-Wpointer-arith", "-Werror", "-Wno-error=pointer-arith")
    assert proc.returncode == 0 and _diags(proc) == EXPECTED, proc.stderr


def test_pragma(tmp_path):
    body = (
        "void *a(void *p) { return p + 1; }\n"
        "#pragma GCC diagnostic push\n"
        '#pragma GCC diagnostic ignored "-Wpointer-arith"\n'
        "void *b(void *p) { return p + 1; }\n"
        "#pragma GCC diagnostic pop\n"
        "void *c(void *p) { return p + 1; }\n"
    )
    for opt in ("-O0", "-O2"):
        assert [d[0] for d in _diags(_run(tmp_path, body, "-Wpointer-arith", opt))] == [1, 6]


def test_safe_default_is_off(tmp_path):
    body = "void * view f(void * view p) { return p + 1; }\n"
    proc = _run(tmp_path, body, compat=False)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr
    proc = _run(tmp_path, body, "-Wpointer-arith", compat=False)
    assert _diags(proc) == [(1, ARITH)], proc.stderr


def test_not_in_a_system_header(tmp_path):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "s.h").write_text("static inline void *nx(void *p) { return p + 1; }\n")
    body = "#include <s.h>\nvoid *g(void *p) { return nx(p); }\n"
    for opt in ("-O0", "-O2"):
        proc = _run(tmp_path, body, "-Wpointer-arith", opt, "-isystem", str(inc))
        assert _diags(proc) == [], proc.stderr


def test_once_in_an_inlined_function(tmp_path):
    body = (
        "static inline void *nx(void *p) { return p + 1; }\n"
        "void *g(void *p) { return nx(nx(p)); }\n"
    )
    for opt in LEVELS:
        assert _diags(_run(tmp_path, body, "-Wpointer-arith", opt)) == [(1, ARITH)]


@pytest.mark.parametrize("opt", LEVELS)
def test_code_unchanged(tmp_path, opt):
    a = _run(tmp_path, BODY, opt, out="a.o")
    b = _run(tmp_path, BODY, opt, "-Wpointer-arith", out="b.o")
    assert a.returncode == 0 and b.returncode == 0
    assert (tmp_path / "a.o").read_bytes() == (tmp_path / "b.o").read_bytes()
