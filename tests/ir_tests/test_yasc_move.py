"""yasc move(p) and borrow(p) (docs/static_analysis_todo.md, "Explicit
move(p) and borrow(p)").

Where ownership leaves an owned variable or member the code says so:
`take(move(p))`, `q = move(p)`, `n->text = move(text)`, `return
move(s->buf)`.  Without it -Wimplicit-move (on by default; yasc only)
warns "ownership of 'p' moves here: write move(p)".  Not needed: an
argument for a `drop` parameter (a deallocator) and `return p;` of a whole
local or parameter.  move(p) is an owned result, like a call's: discarded
it leaks, stored in a view it is never freed.  borrow(p) is a view of p's
memory that p keeps owning; the flow checks follow it as a borrow.  Both take
an owned pointer.  A variable or function named move / borrow in scope wins.

The run-time behaviour is in yasc_move.c (test_qemu.py, every -O level).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
void * owned malloc(unsigned n);
void free(drop void * owned maybe p);
void take(char * owned p);
int use(char *p);
struct box { char * owned buf; char * view peek; };
char * owned g;
#pragma tcc c_begin
void c_use(char *p);
#pragma tcc c_end
"""
PRELUDE_LINES = PRELUDE.count("\n")
LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _check(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-Wlifetime", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error", "note"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[1], parts[2]))
    return proc, out


def _diags(tmp_path, body, *flags):
    proc, out = _check(tmp_path, body, *flags)
    assert proc.returncode == 0, proc.stderr
    return out


def _moves(name):
    return f"ownership of '{name}' moves here: write move({name})"


MEMBER = "ownership moves out of a member or through a pointer here: write move(...)"

NEEDS_MOVE = {
    "owned_parameter": ("void f(void)\n{\n  char * owned p = malloc(1);\n  take(p);\n}\n", 4, _moves("p")),
    "owned_variable": (
        "void f(void)\n{\n  char * owned p = malloc(1);\n  char * owned q = p;\n  free(q);\n}\n",
        4,
        _moves("p"),
    ),
    "assignment": (
        "void f(void)\n{\n  char * owned p = malloc(1);\n  char * owned q;\n  q = p;\n  free(q);\n}\n",
        5,
        _moves("p"),
    ),
    "owned_member": (
        "void f(inout struct box *s)\n{\n  char * owned p = malloc(1);\n  free(s->buf);\n  s->buf = p;\n}\n",
        5,
        _moves("p"),
    ),
    "out_of_member": (
        "void f(inout struct box *s)\n{\n  char * owned p = s->buf;\n  s->buf = 0;\n  free(p);\n}\n",
        3,
        MEMBER,
    ),
    "return_member": ("char * owned f(inout struct box *s)\n{\n  return s->buf;\n}\n", 3, MEMBER),
    "return_global": ("char * owned f(void)\n{\n  return g;\n}\n", 3, _moves("g")),
    "return_static_local": (
        "char * owned f(void)\n{\n  static char * owned keep;\n  return keep;\n}\n",
        4,
        _moves("keep"),
    ),
    "drop_parameter_assigned": (
        "void f(drop char * owned p, char * owned q)\n{\n  free(p);\n  p = q;\n  free(p);\n}\n",
        4,
        _moves("q"),
    ),
}


@pytest.mark.parametrize("name", sorted(NEEDS_MOVE))
def test_needs_move(tmp_path, name):
    body, line, msg = NEEDS_MOVE[name]
    assert (line, "warning", msg) in _diags(tmp_path, body)


@pytest.mark.parametrize("name", sorted(NEEDS_MOVE))
def test_with_move_is_clean(tmp_path, name):
    body, line, msg = NEEDS_MOVE[name]
    lines = body.split("\n")
    stmt = lines[line - 1]
    # wrap the moved value: the right-hand side, the argument or the returned value
    if "take(p)" in stmt:
        stmt = stmt.replace("take(p)", "take(move(p))")
    elif stmt.strip().startswith("return "):
        val = stmt.strip()[len("return ") : -1]
        stmt = stmt.replace(f"return {val};", f"return move({val});")
    else:
        lhs, rhs = stmt.rsplit(" = ", 1)
        stmt = f"{lhs} = move({rhs[:-1]});"
    lines[line - 1] = stmt
    want = []
    if name == "return_member":
        # moved out of a `view` parameter's struct and not replaced: the
        # caller's struct still owns it (the method self rules, test_yasc_self.py)
        want = [(3, "warning", "'s->buf' is moved when 'f' returns: 's' still owns it -- give it a new value")]
    assert [d for d in _diags(tmp_path, "\n".join(lines)) if d[1] != "note"] == want


NO_MOVE_NEEDED = {
    "drop_argument": "void f(void)\n{\n  char * owned p = malloc(1);\n  free(p);\n}\n",
    "return_local": "char * owned f(void)\n{\n  char * owned p = malloc(1);\n  return p;\n}\n",
    "return_parameter": "char * owned f(char * owned p)\n{\n  return p;\n}\n",
    "view_argument": "void f(void)\n{\n  char * owned p = malloc(1);\n  use(p);\n  free(p);\n}\n",
    "view_variable": "void f(void)\n{\n  char * owned p = malloc(1);\n  char * view v = p;\n  use(v);\n  free(p);\n}\n",
    "view_member": "void f(inout struct box *s)\n{\n  s->peek = s->buf;\n}\n",
    "call_result": "void f(void)\n{\n  take(malloc(1));\n  char * owned q = malloc(2);\n  free(q);\n}\n",
    "move_on_drop": "void f(void)\n{\n  char * owned p = malloc(1);\n  free(move(p));\n}\n",
    "move_on_return": "char * owned f(char * owned p)\n{\n  return move(p);\n}\n",
}


@pytest.mark.parametrize("name", sorted(NO_MOVE_NEEDED))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_no_move_needed(tmp_path, name, opt):
    assert _diags(tmp_path, NO_MOVE_NEEDED[name], opt) == []


def test_wno_implicit_move(tmp_path):
    """Off, the flow checks still follow the implicit move."""
    body = "void f(void)\n{\n  char * owned p = malloc(1);\n  take(p);\n  use(p);\n}\n"
    assert _diags(tmp_path, body, "-Wno-implicit-move") == [
        (5, "warning", "'p' is used after it was moved"),
        (4, "note", "'p' was moved here"),
    ]


@pytest.mark.parametrize("opt", LEVELS)
def test_use_after_explicit_move(tmp_path, opt):
    body = "void f(void)\n{\n  char * owned p = malloc(1);\n  auto q = move(p);\n  use(p);\n  free(q);\n}\n"
    assert _diags(tmp_path, body, opt) == [
        (5, "warning", "'p' is used after it was moved"),
        (4, "note", "'p' was moved here"),
    ]


@pytest.mark.parametrize("opt", LEVELS)
def test_borrow_follows_its_owner(tmp_path, opt):
    body = "void f(void)\n{\n  char * owned p = malloc(1);\n  auto v = borrow(p);\n  use(v);\n  free(p);\n  use(v);\n}\n"
    assert _diags(tmp_path, body, opt) == [
        (7, "warning", "'v' is used after 'p', which it borrows from, was freed"),
        (6, "note", "'p' was freed here"),
    ]


def test_borrow_owns_nothing(tmp_path):
    body = "void f(void)\n{\n  char * owned p = malloc(1);\n  take(borrow(p));\n  free(p);\n}\n"
    assert (4, "warning", "a pointer that owns nothing is stored in an owned pointer") in _diags(tmp_path, body)


def test_moved_into_a_view_is_never_freed(tmp_path):
    body = "void f(void)\n{\n  char * owned p = malloc(1);\n  char * view v = move(p);\n  use(v);\n}\n"
    assert (4, "warning", "an owned result is stored in a pointer that owns nothing: it is never freed") in _diags(
        tmp_path, body
    )


def test_move_discarded(tmp_path):
    body = "void f(void)\n{\n  char * owned p = malloc(1);\n  move(p);\n}\n"
    assert (4, "warning", "an owned result is discarded: it is never freed") in _diags(tmp_path, body)


@pytest.mark.parametrize("kw", ["move", "borrow"])
def test_of_a_pointer_that_owns_nothing(tmp_path, kw):
    proc, out = _check(tmp_path, f"void f(char *v)\n{{\n  use({kw}(v));\n}}\n")
    assert proc.returncode != 0
    assert out[0][1:] == ("error", f"{kw} of a pointer that owns nothing")


def test_names_in_scope_win(tmp_path):
    body = (
        "static int move(int x) { return x + 1; }\n"
        "int f(void)\n{\n  int borrow = 2;\n  return move(borrow);\n}\n"
    )
    assert _diags(tmp_path, body) == []


def test_identifiers_outside_yasc(tmp_path):
    src = tmp_path / "c.c"
    src.write_text("int move(int x);\nint borrow;\nint f(void) { return move(borrow); }\n")
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-c", str(src), "-o", str(tmp_path / "c.o")], capture_output=True, text=True
    )
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


def test_auto_with_move_and_borrow(tmp_path):
    body = (
        "void f(void)\n{\n  char * owned p = malloc(1);\n  auto v = borrow(p);\n  use(v);\n"
        "  auto q = move(p);\n  free(q);\n}\n"
    )
    assert _diags(tmp_path, body) == []


# an owned variable handed to a regular-C parameter says borrow(p) or move(p)
C_PARAM = "'p' is owned and goes to a regular-C parameter: write borrow(p) or move(p)"


@pytest.mark.parametrize("opt", LEVELS)
def test_owned_to_c_parameter_says_which(tmp_path, opt):
    body = "void f(void)\n{\n  char * owned p = malloc(1);\n  c_use(p);\n  free(p);\n}\n"
    assert _diags(tmp_path, body, opt) == [(4, "warning", C_PARAM)]


@pytest.mark.parametrize("opt", LEVELS)
def test_borrow_to_c_parameter(tmp_path, opt):
    body = "void f(void)\n{\n  char * owned p = malloc(1);\n  c_use(borrow(p));\n  free(p);\n}\n"
    assert _diags(tmp_path, body, opt) == []


@pytest.mark.parametrize("opt", LEVELS)
def test_move_to_c_parameter(tmp_path, opt):
    """move(p) to a C parameter: p is gone there -- no leak, and freeing it
    afterwards is reported."""
    kept = "void f(void)\n{\n  char * owned p = malloc(1);\n  c_use(move(p));\n}\n"
    assert _diags(tmp_path, kept, opt) == []
    freed = "void f2(void)\n{\n  char * owned p = malloc(1);\n  c_use(move(p));\n  free(p);\n}\n"
    assert _diags(tmp_path, freed, opt) == [
        (5, "warning", "'p' is used after it was moved"),
        (4, "note", "'p' was moved here"),
    ]


C_CTX = "#pragma tcc c_begin\nstruct cctx { char *p; };\n#pragma tcc c_end\nint sink(const struct cctx *c);\n"


@pytest.mark.parametrize("opt", LEVELS)
def test_move_to_c_member(tmp_path, opt):
    """move(p) into a regular-C member (a context the kernel takes, libc
    dlclose): p is gone there, as into a C parameter -- by an initializer or
    an assignment; freeing it afterwards is reported."""
    init = C_CTX + "int f(void)\n{\n  char * owned p = malloc(1);\n  struct cctx c = {.p = move(p)};\n  return sink(&c);\n}\n"
    assert _diags(tmp_path, init, opt) == []
    assign = C_CTX + "int f(void)\n{\n  char * owned p = malloc(1);\n  struct cctx c = undefined;\n  c.p = move(p);\n" \
        "  return sink(&c);\n}\n"
    assert _diags(tmp_path, assign, opt) == []
    freed = C_CTX + "int f(void)\n{\n  char * owned p = malloc(1);\n  struct cctx c = {.p = move(p)};\n  free(p);\n" \
        "  return sink(&c);\n}\n"
    assert _diags(tmp_path, freed, opt) == [
        (9, "warning", "'p' is used after it was moved"),
        (8, "note", "'p' was moved here"),
    ]
    # a drop parameter handed over the same way (libc dlclose)
    param = C_CTX + "int give(drop char * owned h);\nint give(drop char * owned h)\n{\n  struct cctx c = {.p = move(h)};\n" \
        "  return sink(&c);\n}\n"
    assert _diags(tmp_path, param, opt) == []


def test_owned_result_to_c_parameter(tmp_path):
    body = "void f(void)\n{\n  c_use(malloc(1));\n}\n"
    assert _diags(tmp_path, body) == []
