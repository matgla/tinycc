"""-Wextra: the gcc -Wextra checks tcc has, as a group like -Wall.

Members (WD_EXTRA in options_W): implicit-fallthrough, empty-body,
sign-compare, type-limits; unused-parameter and unused-but-set-parameter
only together with -Wunused or -Wall (WD_EXTRA_UNUSED), as gcc.  Not in
-Wall; the safe default has implicit-fallthrough and empty-body anyway.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")


def _run(tmp_path, body, *flags, compat=True):
    src = tmp_path / "t.c"
    src.write_text(body)
    cmd = [str(TCC), "-Wno-missing-prototypes"] + (["-ccompat"] if compat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / "t.o")]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc, kind="warning"):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] == kind:
            out.append((int(parts[0].rsplit(":", 1)[1]), parts[2]))
    return sorted(out)


BODY = """\
int f(int a, unsigned u, int p, int q)
{
  if (a);
  switch (a) { case 1: a++; case 2: a--; }
  q = 1;
  return (a < u) + (u >= 0);
}
"""

EMPTY = (3, "suggest braces around empty body in an 'if' statement")
FALL = (4, "this statement may fall through")
SIGN = (6, "comparison of integer expressions of different signedness: 'int' and 'unsigned int'")
LIMITS = (6, "comparison of unsigned expression in '>= 0' is always true")
PARAM = (1, "unused parameter 'p'")
SETPARAM = (1, "parameter 'q' set but not used")

EXTRA = sorted([EMPTY, FALL, SIGN, LIMITS])


def test_extra_switches_the_group(tmp_path):
    assert _diags(_run(tmp_path, BODY)) == []
    assert _diags(_run(tmp_path, BODY, "-Wall")) == []
    assert _diags(_run(tmp_path, BODY, "-Wextra")) == EXTRA


@pytest.mark.parametrize("flags", [("-Wextra", "-Wunused"), ("-Wunused", "-Wextra"), ("-Wall", "-Wextra"),
                                   ("-Wextra", "-Wall")])
def test_unused_parameters_with_wunused_or_wall(tmp_path, flags):
    proc = _run(tmp_path, BODY, *flags)
    assert _diags(proc) == sorted(EXTRA + [PARAM, SETPARAM]), proc.stderr


def test_member_given_explicitly_wins(tmp_path):
    proc = _run(tmp_path, BODY, "-Wextra", "-Wall", "-Wno-unused-parameter", "-Wno-sign-compare")
    assert _diags(proc) == sorted([EMPTY, FALL, LIMITS, SETPARAM]), proc.stderr
    proc = _run(tmp_path, BODY, "-Wextra", "-Wno-extra")
    assert _diags(proc) == [], proc.stderr


def test_werror_extra(tmp_path):
    proc = _run(tmp_path, BODY, "-Werror=extra")
    assert proc.returncode != 0
    assert _diags(proc, "error")[:1] == [EMPTY], proc.stderr
    proc = _run(tmp_path, BODY, "-Wextra", "-Werror", "-Wno-error=extra")
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == EXTRA, proc.stderr


def test_pragma_extra(tmp_path):
    body = "#pragma GCC diagnostic warning \"-Wextra\"\n" + BODY
    proc = _run(tmp_path, body)
    assert _diags(proc) == [(n + 1, m) for n, m in EXTRA], proc.stderr
    body = "#pragma GCC diagnostic ignored \"-Wextra\"\n" + BODY
    assert _diags(_run(tmp_path, body, "-Wextra")) == []


def test_safe_default_has_extra(tmp_path):
    # the safe default has every -Wextra check, -Wno-extra takes them away
    body = BODY.replace("int f(", "int f(")
    got = _diags(_run(tmp_path, body, compat=False))
    for d in (SIGN, LIMITS, PARAM, SETPARAM):
        assert d in got, got
    proc = _run(tmp_path, body, "-Wno-extra", compat=False)
    assert [d for d in _diags(proc) if d in (SIGN, LIMITS, PARAM, SETPARAM)] == [], proc.stderr
