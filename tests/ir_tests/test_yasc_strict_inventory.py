"""S1 inventory (docs/static_analysis_todo.md, "What the flow checks do not
follow"): lifetime bugs the checker used to miss.

Strict lifetimes need every one of them either reported or rejected.  All
the cases found on 2026-11-23 were closed the same day; the closed ones live
on as tests next to their check (test_lifetime*.py, test_yasc_*.py), and the
last one stays here.  A new gap goes in CASES with
`pytest.param(name, marks=pytest.mark.xfail(strict=True))` until it is closed.

A case passes when its line gets a diagnostic (warning or error) from the
lifetime checks, or the check says it stops following the value ("... the
lifetime check does not follow ..."; S1 makes that an error, S2 adds
`unsafe` as the way past it).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O2"]

PRELUDE = """\
#pragma yasc
void * owned malloc(unsigned n);
void free(drop void * owned maybe p);
int use(char *p);
struct node { int v; struct node * view maybe next; };
struct buf { char * owned maybe p; };
struct node gn;
void reset(void);
void put(char * owned maybe *pp);
struct buf * borrows(static) holder(void);
"""
PRELUDE_LINES = PRELUDE.count("\n")

# name -> (body, line of the bug counted from the body's first line)
CASES = {
    # a call result used as a base is a new value per call
    "call_result_base": (
        "void f(void)\n{\n  free(holder()->p);\n  use(holder()->p);\n}\n",
        4,
    ),
}


def _compile(tmp_path, body, level):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-ccompat", "-Wlifetime", level, "-c", str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _lines_reported(proc):
    lines = set()
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            try:
                lines.add(int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES)
            except ValueError:
                pass
    return lines


@pytest.mark.parametrize("level", LEVELS)
@pytest.mark.parametrize("name", sorted(CASES))
def test_reported_or_rejected(tmp_path, name, level):
    body, bug_line = CASES[name]
    proc = _compile(tmp_path, body, level)
    rejected = "does not follow" in proc.stderr or (proc.returncode != 0 and "unsafe" in proc.stderr)
    assert rejected or bug_line in _lines_reported(proc), proc.stderr


@pytest.mark.parametrize("name", sorted(CASES))
def test_compiles_today(tmp_path, name):
    """Each case is valid C that builds today: the inventory is about what
    the checks miss, not about syntax."""
    proc = _compile(tmp_path, CASES[name][0], "-O0")
    assert proc.returncode == 0, proc.stderr
