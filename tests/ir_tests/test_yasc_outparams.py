"""yasc owned out-parameters (docs/static_analysis_todo.md): a parameter
that points at an owned pointer -- `char *?owned *view lineptr`
(getline: in/out, the callee may replace it), `out void *?owned *
view memptr` (posix_memalign: written, whatever was there is lost).

A variable whose address goes only into such arguments stays followed:
after the call it owns what the callee left there (maybe, when the pointer
is).  out: an owned value already there leaks ("when it is written
through its address").  With a status result (an int), the call's test
decides: 0 / >= 0 is success (written), nonzero / < 0 failure (nothing
owned) -- `if (posix_memalign(&p, ...)) return;` is clean.  An address
taken any other way still makes the variable untracked.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
void free(drop void *?owned p);
long getl(inout char *?owned *lineptr, inout unsigned long *n);
int pma(out void *?owned *memptr, unsigned long sz);
int aspr(out char *?owned *strp, const char *fmt);
void g(inout char *?view *p);
"""
PRELUDE_LINES = PRELUDE.count("\n")
LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _diags(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-Wlifetime", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0, proc.stderr
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "note"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[1], parts[2]))
    return out


GETL = "void f(void)\n{\n  char *?owned line = 0;\n  unsigned long n = 0;\n"

CLEAN = {
    "in_out_freed": GETL + "  getl(&line, &n);\n  free(line);\n}\n",
    "in_out_twice": GETL + "  getl(&line, &n);\n  getl(&line, &n);\n  free(line);\n}\n",
    "out_status_tested": "void f(void)\n{\n  void *?owned m;\n  if (pma(&m, 16))\n    return;\n  free(m);\n}\n",
    "out_status_lt_zero": "void f(void)\n{\n  char *?owned s;\n  if (aspr(&s, \"x\") < 0)\n    return;\n  free(s);\n}\n",
    # the status kept in a local defined once, tested in another block
    "out_status_in_variable": "void f(void)\n{\n  void *?owned m;\n  int r = pma(&m, 16);\n  if (r)\n    return;\n"
    "  free(m);\n}\n",
    # assigned more than once: the call whose result it holds there decides
    "status_variable_reused": "void f(void)\n{\n  void *?owned m;\n  int r = 0;\n  r = pma(&m, 16);\n"
    "  if (r)\n    return;\n  free(m);\n}\n",
    "out_status_assigned_later": "void f(int c)\n{\n  void *?owned m;\n  int r;\n  if (c)\n    c = 2;\n"
    "  r = pma(&m, 16);\n  if (r != 0)\n    return;\n  free(m);\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_clean(tmp_path, name, opt):
    assert _diags(tmp_path, CLEAN[name], opt) == []


BAD = {
    # its address to a parameter that may write it: not followed, and said so
    "address_elsewhere_untracked": (
        GETL + "  g(&line);\n  free(line);\n}\n",
        5,
        "the address of 'line' goes where it may be written: the lifetime check does not follow 'line'",
    ),
    "in_out_leaks": (GETL + "  getl(&line, &n);\n}\n", 6, "'line' leaks: it still owns memory at the end of its scope"),
    "in_out_after_free": (GETL + "  free(line);\n  getl(&line, &n);\n  free(line);\n}\n", 6, "'line' is used after it was freed"),
    "out_overwrites": (
        "void f(void)\n{\n  void *?owned m;\n  if (pma(&m, 16))\n    return;\n  if (pma(&m, 32))\n    return;\n"
        "  free(m);\n}\n",
        6,
        "'m' leaks: it still owns memory when it is written through its address",
    ),
    "out_success_leaks": (
        "void f(void)\n{\n  void *?owned m;\n  if (pma(&m, 16))\n    return;\n}\n",
        6,
        "'m' leaks: it still owns memory at the end of its scope",
    ),
    "out_status_in_variable_leaks": (
        "void f(void)\n{\n  void *?owned m;\n  int r = pma(&m, 16);\n  if (r)\n    return;\n}\n",
        7,
        "'m' leaks: it still owns memory at the end of its scope",
    ),
}


@pytest.mark.parametrize("name", sorted(BAD))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_reported(tmp_path, name, opt):
    body, line, msg = BAD[name]
    assert (line, "warning", msg) in _diags(tmp_path, body, opt)
