"""-Wunused-result: warn_unused_result and C23 [[nodiscard]] / [[nodiscard("why")]].

On by default (as with gcc; -ccompat too) and in -Wall.  The result of such
a function dropped by an expression statement, the left side of a comma, a
for clause or a statement of a statement expression other than its last is
reported; the reason of nodiscard("why") is in the message.  A cast to void
keeps the result -- for both attributes (clang's rule; gcc does not let
(void) silence warn_unused_result).  Once per place at every -O level.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
int wur(void) __attribute__((warn_unused_result));
[[nodiscard]] int nd(void);
[[nodiscard("check it")]] int ndr(int);
[[gnu::warn_unused_result]] int gwur(void);
int __attribute__((__warn_unused_result__)) wur2(void);
int plain(void);
"""
PL = PRELUDE.count("\n")

WUR = "ignoring return value of 'wur', declared with attribute 'warn_unused_result'"
ND = "ignoring return value of 'nd', declared with attribute 'nodiscard'"


def _run(tmp_path, body, *flags, ccompat=True, prelude=PRELUDE):
    src = tmp_path / "t.c"
    src.write_text(prelude + body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / "t.o")]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[1].isdigit():
            out.append((int(parts[1]) - PL, parts[3].strip()))
    return sorted(out)


BODY = """\
static inline int inl(int x) { nd(); return x; }
int use(int c)
{
  int x;
  wur();
  nd();
  ndr(1);
  gwur();
  wur2();
  (void)wur();
  (void)nd();
  x = wur();
  if (nd()) x++;
  wur(), x++;
  for (wur(); c; nd()) c--;
  plain();
  x += ({ nd(); 1; });
  x += ({ 1; nd(); });
  (nd());
  return x + inl(1) + inl(2) + ndr(nd());
}
"""
EXPECTED = [
    (1, ND),
    (5, WUR),
    (6, ND),
    (7, "ignoring return value of 'ndr', declared with attribute 'nodiscard': check it"),
    (8, "ignoring return value of 'gwur', declared with attribute 'warn_unused_result'"),
    (9, "ignoring return value of 'wur2', declared with attribute 'warn_unused_result'"),
    (14, WUR),
    (15, ND),
    (15, WUR),
    (17, ND),
    (19, ND),
]


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_hits_once_at_every_level(tmp_path, opt):
    proc = _run(tmp_path, BODY, opt)
    assert proc.returncode == 0
    assert _diags(proc) == EXPECTED, proc.stderr


def test_switches(tmp_path):
    body = "void t(void) { wur(); }\n"
    assert WUR in _run(tmp_path, body).stderr  # on by default, -ccompat too
    assert WUR in _run(tmp_path, body, "-Wall").stderr
    assert WUR not in _run(tmp_path, body, "-Wno-unused-result").stderr
    assert WUR not in _run(tmp_path, body, "-Wall", "-Wno-unused-result").stderr
    proc = _run(tmp_path, body, "-Werror=unused-result")
    assert proc.returncode != 0 and "error: " + WUR in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Werror", "-Wno-error=unused-result")
    assert proc.returncode == 0 and "warning: " + WUR in proc.stderr, proc.stderr
    assert not _run(tmp_path, body, "-w").stderr


def test_safe_default(tmp_path):
    proc = _run(tmp_path, "void t(void) { nd(); }\n", ccompat=False, prelude="[[nodiscard]] int nd(void);\n")
    assert proc.returncode == 0 and "warning: " + ND in proc.stderr, proc.stderr


def test_pragma(tmp_path):
    body = (
        "void t(void)\n{\n"
        "#pragma GCC diagnostic push\n"
        "#pragma GCC diagnostic ignored \"-Wunused-result\"\n"
        "  wur();\n"
        "#pragma GCC diagnostic pop\n"
        "  wur();\n}\n"
    )
    assert _diags(_run(tmp_path, body)) == [(7, WUR)]


def test_system_header_is_quiet(tmp_path):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "s.h").write_text(PRELUDE + "static inline void sh(void) { wur(); }\n")
    proc = _run(tmp_path, "#include <s.h>\nvoid u(void) { sh(); wur(); }\n", "-isystem", str(inc), prelude="")
    lines = [l for l in proc.stderr.splitlines() if "ignoring return value" in l]
    assert len(lines) == 1 and "t.c:2:" in lines[0], proc.stderr


def test_through_a_pointer_with_the_attribute(tmp_path):
    body = "int (*fp)(void) __attribute__((warn_unused_result));\nvoid t(void) { fp(); }\n"
    assert "ignoring return value of 'fp'" in _run(tmp_path, body).stderr


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_objects_identical(tmp_path, opt):
    a = _run(tmp_path, BODY, opt, "-Wno-unused-result")
    assert a.returncode == 0 and not a.stderr, a.stderr
    obj_a = (tmp_path / "t.o").read_bytes()
    b = _run(tmp_path, BODY, opt)
    assert b.returncode == 0
    assert (tmp_path / "t.o").read_bytes() == obj_a


TYPE_PRELUDE = """\
struct [[nodiscard]] S { int a; };
struct [[nodiscard("must check")]] E2 { int a; };
enum [[nodiscard]] err { OK, BAD };
union [[nodiscard]] U { int a; float f; };
struct __attribute__((warn_unused_result)) W { int a; };
struct S fs(void);
struct E2 fe2(void);
enum err fe(void);
union U fu(void);
typedef struct S S_t;
S_t ft(void);
struct S *fp(void);
struct W fw(void);
[[nodiscard]] struct S fboth(void);
"""
TYPE_BODY = """\
static inline enum err ie(void) { return fe(); }
void g(void)
{
  fs();
  fe2();
  fe();
  fu();
  ft();
  fp();
  fw();
  (void)fs();
  struct S x = fs();
  (void)x;
  fboth();
  ie();
  ie();
}
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_nodiscard_type(tmp_path, opt):
    """C23 [[nodiscard]] on a struct, union or enum: every function returning
    it (gcc's wording); a pointer to it is not it, and warn_unused_result on a
    type means nothing (gcc ignores it)"""
    proc = _run(tmp_path, TYPE_BODY, opt, "-std=c23", prelude=TYPE_PRELUDE)
    assert proc.returncode == 0, proc.stderr
    got = [(l + PL - TYPE_PRELUDE.count("\n"), m) for l, m in _diags(proc)]
    assert got == [
        (4, "ignoring return value of type 'struct S', declared with attribute 'nodiscard'"),
        (5, "ignoring return value of type 'struct E2', declared with attribute 'nodiscard': must check"),
        (6, "ignoring return value of type 'enum err', declared with attribute 'nodiscard'"),
        (7, "ignoring return value of type 'union U', declared with attribute 'nodiscard'"),
        (8, "ignoring return value of type 'struct S', declared with attribute 'nodiscard'"),
        (14, "ignoring return value of 'fboth', declared with attribute 'nodiscard'"),
        (15, "ignoring return value of type 'enum err', declared with attribute 'nodiscard'"),
        (16, "ignoring return value of type 'enum err', declared with attribute 'nodiscard'"),
    ], proc.stderr
    assert _run(tmp_path, TYPE_BODY, opt, "-std=c23", "-Wno-unused-result", prelude=TYPE_PRELUDE).stderr == ""
