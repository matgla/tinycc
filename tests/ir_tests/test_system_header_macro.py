"""Warnings in the expansion of a macro a system header defines are quiet (gcc).

A macro defined in a system header (found on a system include path, or after
its #pragma GCC system_header) is MACRO_SYS; while the parse reads what it
expands to, a warning about that position is not given -- unless
-Wsystem-headers.  -Wshadow looks at where the declared name came from.
Errors are always given.  A body saved as tokens (-O1+) keeps the ranges of
such tokens beside it (tccpp.c tok_str_sys_*), not in its tokens: the
objects do not change, and the diagnostics are the same at every -O.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

OPTS = ("-O0", "-O1", "-O2", "-Os")

SYS_H = """\
#define SYS_SHIFT(x) ((x) << 40)
#define SYS_DECL int count = 0
#define SYS_DIV(a) ((a) / 0)
#define SYS_CMP(v) ((v) == (v))
#define SYS_BAD(x) ((x) + )
"""

PRAGMA_H = """\
#pragma GCC system_header
#define PRAGMA_SHIFT(x) ((x) << 40)
"""

USER_H = """\
#define U_SHIFT(x) ((x) << 40)
#define U_DECL int count = 0
"""

MAIN = """\
#include <sysm.h>
#include <pragma.h>
#include "user/userm.h"
int count;
int s1(int v) { return SYS_SHIFT(v) + PRAGMA_SHIFT(v); }
int u1(int v) { return U_SHIFT(v); }
int f(int v)
{
  { SYS_DECL; v += count; }
  { U_DECL; v += count; }
  v += SYS_DIV(v) + SYS_CMP(v);
  v += (v / 0) + (v == v);
  return v + s1(v) + u1(v);
}
static inline int g(int v) { return SYS_SHIFT(v) + U_SHIFT(v); }
int h(int v) { return g(v) + g(v + 1); }
"""

# what gcc says of MAIN (its lines): only the user header's macros and the
# expressions written out
EXPECT = [
    (6, "warning", "left shift count >= width of type"),
    (10, "warning", "declaration of 'count' shadows a global declaration"),
    (12, "warning", "division by zero"),
    (12, "warning", "self-comparison always evaluates to true"),
    (15, "warning", "left shift count >= width of type"),
]


def _tree(tmp_path, main=MAIN):
    (tmp_path / "sys").mkdir(exist_ok=True)
    (tmp_path / "user").mkdir(exist_ok=True)
    (tmp_path / "sys" / "sysm.h").write_text(SYS_H)
    (tmp_path / "user" / "pragma.h").write_text(PRAGMA_H)
    (tmp_path / "user" / "userm.h").write_text(USER_H)
    (tmp_path / "t.c").write_text(main)


def _run(tmp_path, *flags, sys_dir="-isystem", out="t.o"):
    cmd = [str(TCC), "-ccompat", "-Wall", "-Wshadow", sys_dir, "sys", "-I", "user", *flags, "-c", "t.c", "-o", out]
    return subprocess.run(cmd, capture_output=True, text=True, cwd=tmp_path, timeout=60)


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[1].isdigit() and parts[2].strip() in ("warning", "error"):
            out.append((int(parts[1]), parts[2].strip(), parts[3].strip()))
    return out


@pytest.mark.parametrize("opt", OPTS)
def test_quiet_in_a_system_macro(tmp_path, opt):
    _tree(tmp_path)
    p = _run(tmp_path, opt)
    assert p.returncode == 0, p.stderr
    assert sorted(_diags(p)) == EXPECT, p.stderr


def test_same_at_every_level(tmp_path):
    _tree(tmp_path)
    outs = {o: sorted(_diags(_run(tmp_path, o))) for o in OPTS}
    assert len({tuple(v) for v in outs.values()}) == 1, outs


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_system_headers_option_speaks(tmp_path, opt):
    # -Wsystem-headers: a system header's macro is like any other
    _tree(tmp_path)
    d = _diags(_run(tmp_path, opt, "-Wsystem-headers"))
    shifts = [l for l, _, m in d if "shift count" in m]
    assert sorted(shifts) == [5, 5, 6, 15, 15], d
    assert (9, "warning", "declaration of 'count' shadows a global declaration") in d
    assert (11, "warning", "division by zero") in d


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_not_a_system_header_speaks(tmp_path, opt):
    # the same header on a user include path: every warning is given
    _tree(tmp_path)
    d = _diags(_run(tmp_path, opt, sys_dir="-I"))
    assert len([m for _, _, m in d if "shift count" in m]) == 4, d
    assert (9, "warning", "declaration of 'count' shadows a global declaration") in d


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_werror_quiet_too(tmp_path, opt):
    # a warning not given is no error under -Werror either
    _tree(tmp_path, MAIN.replace("  v += (v / 0) + (v == v);\n", "").replace(
        "#include \"user/userm.h\"\n", "").replace("  { U_DECL; v += count; }\n", "").replace(
        "int u1(int v) { return U_SHIFT(v); }\n", "int u1(int v) { return v; }\n").replace(
        "static inline int g(int v) { return SYS_SHIFT(v) + U_SHIFT(v); }",
        "static inline int g(int v) { return SYS_SHIFT(v); }"))
    p = _run(tmp_path, opt, "-Werror")
    assert p.returncode == 0, p.stderr
    assert not _diags(p), p.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_errors_never_quiet(tmp_path, opt):
    _tree(tmp_path, "#include <sysm.h>\nint f(int v)\n{\n  return SYS_BAD(v);\n}\n")
    p = _run(tmp_path, opt)
    assert p.returncode == 1
    assert [(l, k) for l, k, _ in _diags(p)] == [(4, "error")], p.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_pragma_diagnostic_still_applies(tmp_path, opt):
    main = MAIN.replace("int u1(", "#pragma GCC diagnostic ignored \"-Wshift-count-overflow\"\nint u1(")
    _tree(tmp_path, main)
    d = _diags(_run(tmp_path, opt))
    assert not [m for _, _, m in d if "shift count" in m], d


@pytest.mark.parametrize("opt", OPTS)
def test_objects_do_not_change(tmp_path, opt):
    # the header as a system header or not, warnings on or off: the same
    # object (no mark goes into a saved body, whose length the inliner reads)
    _tree(tmp_path)
    a = _run(tmp_path, opt, out="a.o")
    b = _run(tmp_path, opt, sys_dir="-I", out="b.o")
    c = _run(tmp_path, opt, "-w", out="c.o")
    assert a.returncode == b.returncode == c.returncode == 0
    ao, bo, co = ((tmp_path / n).read_bytes() for n in ("a.o", "b.o", "c.o"))
    assert ao == bo == co
