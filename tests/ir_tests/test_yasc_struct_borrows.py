"""yasc `struct T borrows(text) { ... }`: a struct whose values borrow
(docs/static_analysis_todo.md).

The struct takes names; a member that says `borrows(text)` with one of them
is a view of what the value borrows.  Where the type is used, `struct T
borrows(src)` says what a value borrows: a function's result names its
parameters (or static) -- without it, every view parameter (the
elision), with none an error that asks for borrows(static).  A global or
static of such a type must say `struct T borrows(static)`, and a value
stored where it outlives the function (a global, through a pointer) must
borrow static storage: another global, or a call whose result says
borrows(static).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
struct tokenizer borrows(text) { const char * borrows(text) pos; int n; };
struct tokenizer borrows(src) tok_init(const char * view src);
struct tokenizer tok_from(const char * view src, int k);
struct tokenizer borrows(static) tok_static(void);
struct tokenizer borrows(static) g_tok;
"""
PRELUDE_LINES = PRELUDE.count("\n")
STORED = "a 'struct tokenizer' borrows: stored here it outlives the function (it must borrow static storage)"


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


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_local_value_is_fine(tmp_path, opt):
    body = "void f(const char * view s)\n{\n  struct tokenizer t = tok_init(s);\n  t.pos = s + 1;\n  (void)t;\n}\n"
    assert _diags(tmp_path, body, opt) == []


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_static_value_into_a_global(tmp_path, opt):
    assert _diags(tmp_path, "void f(void)\n{\n  g_tok = tok_static();\n}\n", opt) == []


STORES = {
    "global": "void f(const char * view s)\n{\n  g_tok = tok_init(s);\n}\n",
    "through_a_pointer": "void f(struct tokenizer * view o, const char * view s)\n{\n  *o = tok_from(s, 1);\n}\n",
}


@pytest.mark.parametrize("name", sorted(STORES))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_stored_where_it_outlives(tmp_path, name, opt):
    assert (3, STORED) in _diags(tmp_path, STORES[name], opt)


ERRORS = {
    "global_without_static": ("struct tokenizer g2;", "'g2' outlives every call: write 'struct tokenizer borrows(static)'"),
    "result_without_source": (
        "struct tokenizer h(void);",
        "a 'struct tokenizer' result borrows: write borrows(static), or take a view parameter",
    ),
    "unknown_parameter": ("struct tokenizer borrows(x) h(const char * view s);", "borrows(x): no parameter is called 'x'"),
    "not_borrowing": ("struct other { int a; };\nstruct other borrows(static) z;", "borrows(...): 'struct other' does not borrow"),
    "definition_needs_names": ("struct bad borrows(static) { int a; };", "a struct borrows from names it is given: borrows(text)"),
}


@pytest.mark.parametrize("name", sorted(ERRORS))
def test_errors(tmp_path, name):
    decl, msg = ERRORS[name]
    proc = _compile(tmp_path, decl + "\n")
    assert proc.returncode != 0
    assert [l.split("error: ", 1)[1] for l in proc.stderr.splitlines() if "error: " in l][0] == msg
