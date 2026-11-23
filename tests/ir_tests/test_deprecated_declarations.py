"""-Wdeprecated-declarations: __attribute__((deprecated[("why")])) and C23
[[deprecated[("why")]]] on functions, variables, typedefs, struct/union/enum
tags and enumerators.

On by default (as with gcc; -ccompat too) and in -Wall.  Reported at each use,
"'name' is deprecated[: why]"; never at a declaration of the name itself
(a redeclaration, the definition), nor where a declaration whose specifiers
say deprecated names a deprecated type (in its specifiers or declarators), nor
inside a deprecated tag's own body.  Once per place at every -O level; quiet
in system headers.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
int old_fn(void) __attribute__((deprecated));
int old_fn2(void) __attribute__((deprecated("use new_fn2")));
[[deprecated]] int old_var;
[[deprecated("no")]] int old_fn3(void);
int old_fn4 [[deprecated("x4")]] (void);
typedef int old_t __attribute__((deprecated));
struct __attribute__((deprecated)) olds { int x; struct olds *next; };
struct [[deprecated("s2")]] olds2 { int y; };
enum { A_OK, A_OLD __attribute__((deprecated)), A_OLD2 [[deprecated("e")]] = 5 };
int __attribute__((__deprecated__)) old_fn5(void);
"""
PL = PRELUDE.count("\n")


def _run(tmp_path, body, *flags, ccompat=True, prelude=PRELUDE):
    src = tmp_path / "t.c"
    src.write_text(prelude + body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / "t.o")]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[1].isdigit() and not parts[2].strip() == "note":
            out.append((int(parts[1]) - PL, parts[3].strip()))
    return sorted(out)


BODY = """\
int old_fn(void);
int old_fn(void) { return 1; }
__attribute__((deprecated)) old_t quiet_fn(old_t x);
old_t __attribute__((deprecated)) quiet_var;
old_t loud_fn(void);
static inline int inl(void) { return old_fn5(); }
int use(void)
{
  old_t a = 0;
  struct olds s;
  struct olds2 s2;
  int (*fp)(void) = old_fn;
  return old_fn() + old_fn2() + old_var + old_fn3() + old_fn4() + a + A_OLD + A_OLD2 + A_OK + s.x + s2.y
         + fp() + inl() + inl() + (int)sizeof(old_t);
}
"""
EXPECTED = [
    (5, "'old_t' is deprecated"),
    (6, "'old_fn5' is deprecated"),
    (9, "'old_t' is deprecated"),
    (10, "'olds' is deprecated"),
    (11, "'olds2' is deprecated: s2"),
    (12, "'old_fn' is deprecated"),
    (13, "'A_OLD' is deprecated"),
    (13, "'A_OLD2' is deprecated: e"),
    (13, "'old_fn' is deprecated"),
    (13, "'old_fn2' is deprecated: use new_fn2"),
    (13, "'old_fn3' is deprecated: no"),
    (13, "'old_fn4' is deprecated: x4"),
    (13, "'old_var' is deprecated"),
    (14, "'old_t' is deprecated"),
]


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_hits_once_at_every_level(tmp_path, opt):
    proc = _run(tmp_path, BODY, opt)
    assert proc.returncode == 0
    assert _diags(proc) == EXPECTED, proc.stderr


def test_declarations_alone_are_quiet(tmp_path):
    assert not _run(tmp_path, "").stderr


def test_switches(tmp_path):
    body = "int t(void) { return old_fn(); }\n"
    msg = "'old_fn' is deprecated"
    assert msg in _run(tmp_path, body).stderr  # on by default, -ccompat too
    assert msg in _run(tmp_path, body, "-Wall").stderr
    assert msg not in _run(tmp_path, body, "-Wno-deprecated-declarations").stderr
    proc = _run(tmp_path, body, "-Werror=deprecated-declarations")
    assert proc.returncode != 0 and "error: " + msg in proc.stderr, proc.stderr
    proc = _run(tmp_path, body, "-Werror", "-Wno-error=deprecated-declarations")
    assert proc.returncode == 0 and "warning: " + msg in proc.stderr, proc.stderr
    assert not _run(tmp_path, body, "-w").stderr


def test_safe_default(tmp_path):
    proc = _run(tmp_path, "int t(void) { return old(); }\n", ccompat=False,
                prelude="[[deprecated(\"use new\")]] int old(void);\n")
    assert proc.returncode == 0 and "warning: 'old' is deprecated: use new" in proc.stderr, proc.stderr


def test_pragma(tmp_path):
    body = (
        "int t(void)\n{\n"
        "#pragma GCC diagnostic push\n"
        "#pragma GCC diagnostic ignored \"-Wdeprecated-declarations\"\n"
        "  int a = old_fn();\n"
        "#pragma GCC diagnostic pop\n"
        "  return a + old_fn();\n}\n"
    )
    assert _diags(_run(tmp_path, body)) == [(7, "'old_fn' is deprecated")]


def test_system_header(tmp_path):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "s.h").write_text(PRELUDE + "static inline int sh(void) { return old_fn(); }\n")
    proc = _run(tmp_path, "#include <s.h>\nint u(void) { return sh() + old_fn(); }\n", "-isystem", str(inc),
                prelude="")
    lines = [l for l in proc.stderr.splitlines() if "deprecated" in l]
    # used in the header: quiet; used here: reported
    assert len(lines) == 1 and "t.c:2:" in lines[0], proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_objects_identical(tmp_path, opt):
    a = _run(tmp_path, BODY, opt, "-Wno-deprecated-declarations")
    assert a.returncode == 0 and not a.stderr, a.stderr
    obj_a = (tmp_path / "t.o").read_bytes()
    b = _run(tmp_path, BODY, opt)
    assert b.returncode == 0
    assert (tmp_path / "t.o").read_bytes() == obj_a


MEMBERS = """\
struct S {
  int a __attribute__((deprecated));
  int b [[deprecated("use c")]], c;
  [[deprecated]] int d, e;
  struct { int x __attribute__((deprecated)); };
};
static inline int inl_m(struct S *p) { return p->d; }
int f(struct S *p, struct S s)
{
  int r = p->a + s.b + s.c + p->d + p->e + p->x;
  struct S t = { .a = 1 };
  p->a = 2;
  return r + t.c + inl_m(p) + inl_m(&s);
}
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_struct_members(tmp_path, opt):
    """a deprecated member is reported at each use (not in a designated
    initializer, as gcc); after a declarator `deprecated` is only its own"""
    proc = _run(tmp_path, MEMBERS, opt, prelude="")
    assert proc.returncode == 0, proc.stderr
    assert [(l + PL, m) for l, m in _diags(proc)] == [
        (7, "'d' is deprecated"),
        (10, "'a' is deprecated"),
        (10, "'b' is deprecated: use c"),
        (10, "'d' is deprecated"),
        (10, "'e' is deprecated"),
        (10, "'x' is deprecated"),
        (12, "'a' is deprecated"),
    ], proc.stderr


def _notes(proc):
    out = []
    for line in proc.stderr.splitlines():
        if ": note: declared here" in line:
            parts = line.split(":")
            out.append((Path(parts[0]).name, int(parts[1])))
    return out


def test_declared_here_note(tmp_path):
    """each warning is followed by gcc's "declared here" at the declaration,
    in the header that declares it; a warning not given drops its note"""
    proc = _run(tmp_path, MEMBERS, prelude="")
    assert sorted(_notes(proc)) == [("t.c", 2), ("t.c", 2), ("t.c", 3), ("t.c", 4), ("t.c", 4), ("t.c", 4),
                                    ("t.c", 5)], proc.stderr
    (tmp_path / "h.h").write_text("struct [[deprecated]] T { int x; };\n\nint old(void) __attribute__((deprecated));\n")
    body = '#include "h.h"\nint g(void)\n{\n  struct T t = {0};\n  return old() + t.x;\n}\n'
    proc = _run(tmp_path, body, prelude="")
    assert _notes(proc) == [("h.h", 1), ("h.h", 3)], proc.stderr
    quiet = ('#include "h.h"\nint g(void)\n{\n#pragma GCC diagnostic push\n'
             '#pragma GCC diagnostic ignored "-Wdeprecated-declarations"\n  int r = old();\n'
             '#pragma GCC diagnostic pop\n  return r;\n}\n')
    assert _run(tmp_path, quiet, prelude="").stderr == ""
