"""yasc `borrows(...)` on the pointer an out parameter points to:

    long sl(const char * view s, out const char * borrows(s) * view maybe end, int base);

what the callee writes into *end points into s (strtol's end pointer).  The
names are the function's parameters, before or after this one; `static` says
static storage.  Only an `out` parameter's target says it -- an in/out one
would hand back what it read, borrowing from wherever that came from.

The callee may store a pointer into s through end (without it, the view
escape check refuses that store); a pointer into anything else still escapes.
The caller's variable whose address it passed borrows, after the call, from
the owner passed for s: used after that owner was freed, it is reported as a
copy of strchr's result is.
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
char * owned maybe dup(const char * view s);
long sl(const char * view s, out const char * borrows(s) * view maybe end, int base);
long sl2(out char * borrows(s) * view end, inout char * view s);
void name(int e, out const char * borrows(static) * view msg);
void use(const char * view p);
"""
PRELUDE_LINES = PRELUDE.count("\n")
LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _run(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-ccompat", "-Wlifetime", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _diags(tmp_path, body, *flags):
    proc = _run(tmp_path, body, *flags)
    assert proc.returncode == 0, proc.stderr
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "note") and "no previous prototype" not in parts[2]:
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[1], parts[2]))
    return out


OWNER = "  char * owned maybe buf = dup(\"12x\");\n  if (!buf)\n    return 0;\n"

CLEAN = {
    # the callee: a pointer into s stored through end
    "callee_stores_s": "long sl(const char * view s, out const char * borrows(s) * view maybe end, int base)\n"
    "{\n  if (end)\n    *end = s + base;\n  return 0;\n}\n",
    "callee_names_later_param": "long sl2(out char * borrows(s) * view end, inout char * view s)\n{\n  *end = s;\n  return 0;\n}\n",
    "callee_static": "void name(int e, out const char * borrows(static) * view msg)\n{\n  *msg = e ? \"a\" : \"b\";\n}\n",
    # the caller: the end used while its owner lives
    "end_used_before_free": "int f(void)\n{\n" + OWNER + "  const char * view end;\n  sl(buf, &end, 10);\n"
    "  int c = *end;\n  free(buf);\n  return c;\n}\n",
    "no_end": "int f(void)\n{\n" + OWNER + "  long v = sl(buf, nullptr, 10);\n  free(buf);\n  return (int)v;\n}\n",
    # the source a view, not an owned variable: nothing to follow
    "source_is_view": "int f(const char * view s)\n{\n  const char * view end;\n  sl(s, &end, 10);\n"
    "  return *end;\n}\n",
    "static_end": "int f(void)\n{\n  const char * view m;\n  name(1, &m);\n  use(m);\n  return 0;\n}\n",
    # written again from another owner: borrows from that one
    "rebound": "int f(void)\n{\n" + OWNER + "  char * owned maybe b2 = dup(\"7\");\n  if (!b2) {\n"
    "    free(buf);\n    return 0;\n  }\n  const char * view end;\n  sl(buf, &end, 10);\n  sl(b2, &end, 10);\n"
    "  free(buf);\n  int c = *end;\n  free(b2);\n  return c;\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_clean(tmp_path, name, opt):
    assert _diags(tmp_path, CLEAN[name], opt) == []


UAF = "'end' is used after 'buf', which it borrows from, was freed"

BAD = {
    "end_used_after_free": (
        "int f(void)\n{\n" + OWNER + "  const char * view end;\n  sl(buf, &end, 10);\n  free(buf);\n"
        "  return *end;\n}\n",
        [(9, "warning", UAF), (8, "note", "'buf' was freed here")],
    ),
    "end_passed_after_free": (
        "int f(void)\n{\n" + OWNER + "  char * view end;\n  sl2(&end, buf);\n  free(buf);\n  use(end);\n"
        "  return 0;\n}\n",
        [(9, "warning", UAF), (8, "note", "'buf' was freed here")],
    ),
    # a copy of the end borrows what it borrows
    "copy_used_after_free": (
        "int f(void)\n{\n" + OWNER + "  const char * view end;\n  sl(buf, &end, 10);\n"
        "  const char * view p = end + 1;\n  free(buf);\n  return *p;\n}\n",
        [(10, "warning", "'p' is used after 'buf', which it borrows from, was freed"),
         (9, "note", "'buf' was freed here")],
    ),
    # the callee: a pointer into another parameter still escapes
    "callee_stores_other": (
        "long sl(const char * view s, out const char * borrows(s) * view maybe end, int base)\n{\n"
        "  return 0;\n}\nvoid h(const char * view s, const char * view t, out const char * borrows(s) * view e)\n"
        "{\n  *e = t;\n}\n",
        [(7, "warning", "'t' is a view: it cannot leave the function (it is stored through 'e')")],
    ),
}


@pytest.mark.parametrize("name", sorted(BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_bad(tmp_path, name, opt):
    body, want = BAD[name]
    assert _diags(tmp_path, body, opt) == want


ERRORS = {
    "not_out": ("void g(const char * view s, const char * borrows(s) * view e);\n",
                "borrows(...) on the pointer a parameter points to needs out"),
    "three_levels": ("void g(const char * view s, out const char * borrows(s) * view * view e);\n",
                     "borrows(...) qualifies the outermost pointer"),
    "local": ("void g(void)\n{\n  const char * borrows(s) * view e;\n}\n",
              "borrows(...) qualifies the outermost pointer"),
    "no_such_param": ("void g(const char * view s, out const char * borrows(t) * view e);\n",
                      "borrows(t): no parameter is called 't'"),
    "not_a_pointer": ("void g(int n, out const char * borrows(n) * view e);\n", "borrows(n): 'n' is not a pointer"),
    "itself": ("void g(out const char * borrows(e) * view e);\n",
               "borrows(e): an out parameter's target does not borrow from the parameter itself"),
}


@pytest.mark.parametrize("name", sorted(ERRORS))
def test_errors(tmp_path, name):
    body, msg = ERRORS[name]
    proc = _run(tmp_path, body)
    assert proc.returncode != 0
    assert msg in proc.stderr, proc.stderr
