"""-Wlifetime: owned array elements filled and released by loops with a
variable bound (S1, docs/static_analysis_todo.md).

`for (k = C0; k < n; k++) a[k] = alloc(...)` sets the elements C0..n-1 (n
not written in the loop).  A later loop over the same range -- any index
variable, the same bound or a variable equal to it (`m = n`, neither written
since), the same start -- that hands each element to the deallocator its
type names (`owned(fclose)`; plain `owned` is free) releases exactly those.
Without it the elements leak; a bound changed between the loops, another
bound or another start leaves some of them owned.

The range is a state entry of its own (lifetime.c sc_find_loops, exact 2):
the loops over the same class, first element, bound and test share it; an
element reached at a constant index folds it into the elements.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

PRELUDE = """\
#pragma yasc
void * owned maybe malloc(unsigned n);
void free(drop void * owned maybe p);
typedef struct FILE FILE;
FILE * owned(fclose) maybe fopen(const char *path, const char *mode);
int fclose(drop FILE * owned maybe f);
"""
PRELUDE_LINES = PRELUDE.count("\n")

ALLOC = "  for (int k = 0; k < n; k++)\n    a[k] = malloc(1);\n"

CLEAN = {
    "same_bound": "void f(int n)\n{\n  char * owned maybe a[8] = undefined;\n" + ALLOC
    + "  for (int k = 0; k < n; k++)\n    free(a[k]);\n}\n",
    "other_index": "void f(int n)\n{\n  char * owned maybe a[8] = undefined;\n" + ALLOC
    + "  for (int j = 0; j < n; j++)\n    free(a[j]);\n}\n",
    "alias_bound": "void f(int n)\n{\n  char * owned maybe a[8] = undefined;\n  int m = n;\n" + ALLOC
    + "  for (int j = 0; j < m; j++)\n    free(a[j]);\n}\n",
    "deallocator_from_type": "void f(int n)\n{\n  FILE * owned(fclose) maybe a[8] = undefined;\n"
    "  for (int k = 0; k < n; k++)\n    a[k] = fopen(\"x\", \"r\");\n"
    "  for (int j = 0; j < n; j++)\n    fclose(a[j]);\n}\n",
    "used_between": "int use(char * maybe p);\nvoid f(int n)\n{\n  char * owned maybe a[8] = undefined;\n" + ALLOC
    + "  for (int k = 0; k < n; k++)\n    use(a[k]);\n  for (int k = 0; k < n; k++)\n    free(a[k]);\n}\n",
}

# name -> (body, line of the expected report counted from the body's first line)
REPORTED = {
    "no_release": ("void f(int n)\n{\n  char * owned maybe a[8] = undefined;\n" + ALLOC + "}\n", 6),
    "bound_changed": (
        "void f(int n)\n{\n  char * owned maybe a[8] = undefined;\n" + ALLOC
        + "  n++;\n  for (int k = 0; k < n; k++)\n    free(a[k]);\n}\n",
        9,
    ),
    "other_bound": (
        "void f(int n, int m)\n{\n  char * owned maybe a[8] = undefined;\n" + ALLOC
        + "  for (int k = 0; k < m; k++)\n    free(a[k]);\n}\n",
        8,
    ),
    "other_start": (
        "void f(int n)\n{\n  char * owned maybe a[8] = undefined;\n" + ALLOC
        + "  for (int k = 1; k < n; k++)\n    free(a[k]);\n}\n",
        8,
    ),
    "released_twice": (
        "void f(int n)\n{\n  char * owned maybe a[8] = undefined;\n" + ALLOC
        + "  for (int k = 0; k < n; k++)\n    free(a[k]);\n  for (int j = 0; j < n; j++)\n    free(a[j]);\n}\n",
        9,
    ),
    "wrong_deallocator": (
        "void f(int n)\n{\n  FILE * owned(fclose) maybe a[8] = undefined;\n"
        "  for (int k = 0; k < n; k++)\n    a[k] = fopen(\"x\", \"r\");\n"
        "  for (int j = 0; j < n; j++)\n    free(a[j]);\n}\n",
        7,
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


def _reports(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[2]))
    return out


@pytest.mark.parametrize("level", LEVELS)
@pytest.mark.parametrize("name", sorted(CLEAN))
def test_clean(tmp_path, name, level):
    proc = _compile(tmp_path, CLEAN[name], level)
    assert proc.returncode == 0, proc.stderr
    assert _reports(proc) == [], proc.stderr


@pytest.mark.parametrize("level", LEVELS)
@pytest.mark.parametrize("name", sorted(REPORTED))
def test_reported(tmp_path, name, level):
    body, line = REPORTED[name]
    proc = _compile(tmp_path, body, level)
    assert line in [l for l, _ in _reports(proc)], proc.stderr
