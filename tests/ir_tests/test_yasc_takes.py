"""yasc `takes(p)` on a result (docs/static_analysis_todo.md): the result
takes over what the owned parameter p owned -- not necessarily the same
pointer (realloc may move the block).

With a `maybe` result, success is the result's own test: valid -> p was
consumed; null -> p is untouched and still the caller's.  Without `maybe`, p
always moves.  Caller: `q = realloc(p, n); if (!q) { free(p); ... }` is
clean; using p where q is valid is reported, `p = realloc(p, n)` may leak p.
Callee: a path returning null must leave p to the caller ("'f' returns null
but 'p' was freed"); a valid path must have consumed it (the usual leak).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
void free(drop void * owned maybe p);
void * owned maybe malloc(unsigned n);
void * owned maybe takes(p) grow(void * owned p, unsigned n);
void * owned takes(p) wrap(void * owned p);
int use(void * view p);
"""
PRELUDE_LINES = PRELUDE.count("\n")
START = "void f(unsigned n)\n{\n  void * owned maybe p = malloc(4);\n  if (!p)\n    return;\n"


def _compile(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-ccompat", "-Wlifetime", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _diags(tmp_path, body, *flags):
    proc = _compile(tmp_path, body, *flags)
    assert proc.returncode == 0, proc.stderr
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] == "warning":
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[2]))
    return out


LEVELS = ["-O0", "-O1", "-O2", "-Os"]

CALLER_CLEAN = {
    "freed_on_failure": START + "  void * owned maybe q = grow(move(p), n);\n  if (!q)\n  {\n    free(p);\n    return;\n  }\n"
    "  free(q);\n}\n",
    "tested_the_other_way": START + "  void * owned maybe q = grow(move(p), n);\n  if (q)\n    free(q);\n  else\n    free(p);\n}\n",
    "no_maybe_always_moves": START + "  void * owned q = wrap(move(p));\n  free(q);\n}\n",
}


@pytest.mark.parametrize("name", sorted(CALLER_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_caller_clean(tmp_path, name, opt):
    assert _diags(tmp_path, CALLER_CLEAN[name], opt) == []


CALLER_BAD = {
    "used_where_valid": (
        START + "  void * owned maybe q = grow(move(p), n);\n  if (q)\n    use(p);\n  free(q);\n  free(p);\n}\n",
        8,
        "'p' is used after it was moved",
    ),
    "assigned_back": (
        START + "  p = grow(move(p), n);\n  free(p);\n}\n",
        6,
        "'p' leaks: it may still own memory when it is assigned again",
    ),
    "failure_path_leaks": (
        START + "  void * owned maybe q = grow(move(p), n);\n  if (!q)\n    return;\n  free(q);\n}\n",
        8,
        "'p' leaks: it still owns memory at return",
    ),
}


@pytest.mark.parametrize("name", sorted(CALLER_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_caller_reported(tmp_path, name, opt):
    body, line, msg = CALLER_BAD[name]
    assert (line, msg) in _diags(tmp_path, body, opt)


CALLEE = "void * owned maybe takes(p) g(void * owned p, unsigned n)\n{\n  void * owned maybe q = malloc(n);\n"


def test_callee_clean(tmp_path):
    body = CALLEE + "  if (!q)\n    return 0;\n  free(p);\n  return q;\n}\n"
    assert _diags(tmp_path, body) == []


def test_callee_frees_on_failure(tmp_path):
    body = CALLEE + "  if (!q)\n  {\n    free(p);\n    return 0;\n  }\n  free(p);\n  return q;\n}\n"
    assert (7, "'g' returns null but 'p' was freed: takes(p) leaves it to the caller") in _diags(tmp_path, body)


def test_callee_keeps_it_on_success(tmp_path):
    body = CALLEE + "  if (!q)\n    return 0;\n  return q;\n}\n"
    assert (6, "'p' leaks: it still owns memory at return") in _diags(tmp_path, body)


ERRORS = {
    "not_owned_result": ("void * view maybe takes(p) f(void * owned p, char * view s);", "takes(p): the result must be owned"),
    "unknown": ("void * owned takes(x) f(void * owned p);", "takes(x): no parameter is called 'x'"),
    "view_parameter": ("void * owned takes(p) f(void * view p);", "takes(p): 'p' must be an owned parameter that is not drop"),
    "drop_parameter": (
        "void * owned takes(p) f(drop void * owned p);",
        "takes(p): 'p' must be an owned parameter that is not drop",
    ),
    "not_a_function": ("void * owned takes(p) v;", "takes(p): only a function's result takes a parameter over"),
}


@pytest.mark.parametrize("name", sorted(ERRORS))
def test_errors(tmp_path, name):
    decl, msg = ERRORS[name]
    proc = _compile(tmp_path, decl + "\n")
    assert proc.returncode != 0
    assert [l.split("error: ", 1)[1] for l in proc.stderr.splitlines() if "error: " in l][0] == msg
