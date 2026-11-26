"""-Wlifetime: yasc ownership followed through each function (lifetime.c).

An owned local or parameter owns its memory until it is handed on: assigned
to another owned variable, passed to an owned parameter (`drop`: freed) or
returned as an owned result.  The check reports a variable used or handed on
again after that, and one that still owns memory when it is assigned again,
when its scope ends (also by break/continue) or at a return.  States are per
path; where paths with different states meet the message says "may".  It
runs on the IR as the frontend wrote it, before any pass, so the diagnostics
are the same at every -O level, and it never changes the generated code.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

# Ownership tests allocate with a malloc that never returns null; the maybe
# tests use find().
PRELUDE = """\
#pragma yasc
void * owned malloc(unsigned n);
void free(drop void * owned maybe p);
int use(char *p);
int sink(int x);
void take(char * owned p);
void (*take_fp)(char * owned p);
struct big { int a[8]; };
struct big take_big(char * owned p);
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _compile(tmp_path, body, *flags, obj="t.o"):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-ccompat", "-Wlifetime", "-c", *flags, str(src), "-o", str(tmp_path / obj)],
        capture_output=True,
        text=True,
    )


def _diags(proc):
    """(line in the body, kind, message), the body's first line being 1."""
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error", "note"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[1], parts[2]))
    return out


def _check(tmp_path, body, *flags):
    proc = _compile(tmp_path, body, *flags)
    assert proc.returncode == 0, proc.stderr
    return _diags(proc)


# --- what it reports -------------------------------------------------------


def test_use_after_free(tmp_path):
    assert _check(tmp_path, "void f(char * owned p)\n{\n  free(p);\n  use(p);\n}\n") == [
        (4, "warning", "'p' is used after it was freed"),
        (3, "note", "'p' was freed here"),
    ]


def test_double_free(tmp_path):
    assert _check(tmp_path, "void f(char * owned p)\n{\n  free(p);\n  free(p);\n}\n") == [
        (4, "warning", "'p' is freed twice"),
        (3, "note", "'p' was freed here"),
    ]


def test_use_after_move_into_owned_variable(tmp_path):
    body = "void f(char * owned p)\n{\n  char * owned q = move(p);\n  use(p);\n  free(q);\n}\n"
    assert _check(tmp_path, body) == [
        (4, "warning", "'p' is used after it was moved"),
        (3, "note", "'p' was moved here"),
    ]


def test_use_after_move_into_parameter(tmp_path):
    body = "void f(char * owned p)\n{\n  take(move(p));\n  use(p);\n}\n"
    assert _check(tmp_path, body) == [
        (4, "warning", "'p' is used after it was moved"),
        (3, "note", "'p' was moved here"),
    ]


def test_maybe_moved(tmp_path):
    body = "void f(char * owned p, int x)\n{\n  if (x)\n    take(move(p));\n  use(p);\n}\n"
    assert _check(tmp_path, body) == [
        (5, "warning", "'p' may be used after it was moved"),
        (4, "note", "'p' was moved here"),
    ]


def test_leak_at_return(tmp_path):
    body = "void f(int x)\n{\n  char * owned p = malloc(1);\n  if (x)\n    return;\n  free(p);\n}\n"
    assert _check(tmp_path, body) == [(5, "warning", "'p' leaks: it still owns memory at return")]


def test_leak_at_return_value(tmp_path):
    body = "int f(int x)\n{\n  char * owned p = malloc(1);\n  if (x)\n    return 1;\n  free(p);\n  return 0;\n}\n"
    assert _check(tmp_path, body) == [(5, "warning", "'p' leaks: it still owns memory at return")]


def test_leak_at_scope_end(tmp_path):
    body = "void f(void)\n{\n  {\n    char * owned p = malloc(1);\n    use(p);\n  }\n  sink(0);\n}\n"
    assert _check(tmp_path, body) == [(6, "warning", "'p' leaks: it still owns memory at the end of its scope")]


def test_leak_on_one_path(tmp_path):
    body = "void f(int x)\n{\n  char * owned p = malloc(1);\n  if (x)\n    free(p);\n}\n"
    assert _check(tmp_path, body) == [(6, "warning", "'p' leaks: it still owns memory at the end of its scope")]


def test_leak_may_at_join(tmp_path):
    body = (
        "void f(int x)\n{\n  {\n    char * owned p = malloc(1);\n    if (x)\n      free(p);\n    else\n"
        "      use(p);\n  }\n  sink(0);\n}\n"
    )
    assert _check(tmp_path, body) == [(9, "warning", "'p' leaks: it may still own memory at the end of its scope")]


def test_leak_when_assigned_again(tmp_path):
    body = "void f(void)\n{\n  char * owned p = malloc(1);\n  p = malloc(2);\n  free(p);\n}\n"
    assert _check(tmp_path, body) == [(4, "warning", "'p' leaks: it still owns memory when it is assigned again")]


def test_owned_parameter_leaks(tmp_path):
    assert _check(tmp_path, "void f(char * owned p)\n{\n  use(p);\n}\n") == [
        (4, "warning", "'p' leaks: it still owns memory at the end of the function")
    ]


def test_leak_in_loop_body(tmp_path):
    body = (
        "void f(int n)\n{\n  for (int i = 0; i < n; i++)\n  {\n    char * owned t = malloc(2);\n    use(t);\n"
        "  }\n}\n"
    )
    assert _check(tmp_path, body) == [(7, "warning", "'t' leaks: it still owns memory at the end of its scope")]


def test_break_and_continue_end_the_body_scope(tmp_path):
    body = (
        "void f(int n)\n{\n  for (int i = 0; i < n; i++)\n  {\n    char * owned t = malloc(2);\n"
        "    if (i == 3)\n      break;\n    if (i & 1)\n      continue;\n    free(t);\n  }\n}\n"
    )
    assert _check(tmp_path, body) == [
        (7, "warning", "'t' leaks: it still owns memory at the end of its scope"),
        (9, "warning", "'t' leaks: it still owns memory at the end of its scope"),
    ]


def test_use_after_move_into_owned_member(tmp_path):
    body = "struct box { char * owned p; };\nvoid f(inout struct box *b, char * owned p)\n{\n  b->p = move(p);\n  free(p);\n}\n"
    assert _check(tmp_path, body) == [
        (5, "warning", "'p' is used after it was moved"),
        (4, "note", "'p' was moved here"),
    ]


def test_error_union_error_path_leaks(tmp_path):
    body = "!char * owned f(int x)\n{\n  char * owned p = malloc(1);\n  if (x)\n    return error.Bad;\n  return p;\n}\n"
    assert _check(tmp_path, body) == [(5, "warning", "'p' leaks: it still owns memory at return")]


def test_owned_returned_through_plain_pointer_leaks(tmp_path):
    body = "char * borrows(static) f(void)\n{\n  char * owned p = malloc(1);\n  return p;\n}\n"
    assert _check(tmp_path, body) == [(4, "warning", "'p' leaks: it still owns memory at return")]


# --- what is clean ---------------------------------------------------------

CLEAN = {
    "null_test": "void f(char * owned p)\n{\n  if (p)\n    free(p);\n}\n",
    "early_return_on_null": "void f(char * owned p)\n{\n  if (!p)\n    return;\n  free(p);\n}\n",
    "compare_null": "void f(char * owned p)\n{\n  if (p != 0)\n    take(move(p));\n}\n",
    "returned": "char * owned f(void)\n{\n  char * owned p = malloc(1);\n  use(p);\n  return p;\n}\n",
    "moved_then_freed": "void f(char * owned p)\n{\n  char * owned q;\n  q = move(p);\n  use(q);\n  free(q);\n}\n",
    "loop": "void f(int n)\n{\n  for (int i = 0; i < n; i++)\n  {\n    char * owned t = malloc(2);\n"
    "    free(t);\n  }\n}\n",
    "function_pointer": "void f(char * owned p)\n{\n  take_fp(move(p));\n}\n",
    "struct_return_call": "void f(char * owned p)\n{\n  struct big b = take_big(move(p));\n  (void)b;\n}\n",
    "null_then_maybe": "void f(int n)\n{\n  char * owned maybe p = 0;\n  if (n)\n    p = malloc(3);\n  free(p);\n}\n",
    "reassigned_after_free": "void f(void)\n{\n  char * owned p = malloc(1);\n  free(p);\n  p = malloc(2);\n"
    "  free(p);\n}\n",
    "defer": "void f(int x)\n{\n  char * owned p = malloc(1);\n  defer free(p);\n  if (x)\n    return;\n"
    "  use(p);\n}\n",
    "switch_break": "void f(int x)\n{\n  char * owned p = malloc(1);\n  switch (x)\n  {\n  case 1:\n"
    "    break;\n  default:\n    use(p);\n  }\n  free(p);\n}\n",
    "borrowed_copy": "void f(char * owned p)\n{\n  char * view q = p;\n  use(q);\n  free(p);\n}\n",
    # an owned out-parameter: p owns what get left there (test_yasc_outparams.py)
    "address_taken": "void get(char * owned *pp);\nvoid f(void)\n{\n  char * owned p = undefined;\n  get(&p);\n"
    "  use(p);\n  free(p);\n}\n",
    "noreturn": "_Noreturn void die(void);\nvoid f(char * owned p)\n{\n  if (!use(p))\n    die();\n"
    "  free(p);\n}\n",
    # a store into an owned member moves (vstore notes it: lifetime.c)
    "into_owned_member": "struct box { char * owned p; };\nvoid f(inout struct box *b, char * owned p)\n{\n"
    "  b->p = move(p);\n}\n",
    # a store into a view member only borrows
    # a view of p kept in *b while p moves on (freeing p there is
    # test_yasc_view.py's: the view outlives it)
    "into_view_member": "struct pin { char * view p; };\nvoid f(inout struct pin *b, char * owned p)\n{\n"
    "  b->p = p;\n  take(move(p));\n}\n",
    # returned through an error union: the payload is owned
    "returned_in_error_union": "!char * owned make_errunion(int x)\n{\n  char * owned p = malloc(1);\n"
    "  errdefer free(p);\n"
    "  if (x)\n    return error.Bad;\n  return p;\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_clean(tmp_path, name, opt):
    assert _check(tmp_path, CLEAN[name], opt) == []


# --- inlining does not change the answer -----------------------------------

INLINED = """\
static void consume(char * owned p)
{
  free(p);
}

static char * owned make(int n)
{
  char * owned p = malloc(n);
  return p;
}

static int borrow(char *p)
{
  return use(p) + 1;
}

void inl_ok(void)
{
  char * owned p = make(4);
  borrow(p);
  consume(move(p));
}

void inl_bad(int x)
{
  char * owned p = make(4);
  consume(move(p));
  borrow(p);
}

void maybe_moved(int x)
{
  char * owned p = malloc(1);
  if (x)
    consume(move(p));
  use(p);
}
"""


def test_inlined_callees(tmp_path):
    assert _check(tmp_path, INLINED, "-O0") == [
        (28, "warning", "'p' is used after it was moved"),
        (27, "note", "'p' was moved here"),
        (36, "warning", "'p' may be used after it was moved"),
        (35, "note", "'p' was moved here"),
    ]


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_same_at_every_level(tmp_path, opt):
    corpus = INLINED + "".join(body.replace("void f(", f"void f_{name}(") for name, body in CLEAN.items())
    assert _check(tmp_path, corpus, opt) == _check(tmp_path, corpus, "-O0")


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_inline_only_static_is_checked(tmp_path, opt):
    """-O1+: every call to leaky is inlined and nothing compiles it on its own;
    a check-only parse at the end of the unit checks it (once: the expansion
    stays silent).  An uncalled static of the main file is checked too."""
    body = (
        "static void leaky(char * owned p)\n{\n  use(p);\n}\nvoid g(char * owned q)\n{\n  leaky(move(q));\n}\n"
        "static void unused(char * owned p)\n{\n  free(p);\n  free(p);\n}\n"
    )
    assert sorted(_check(tmp_path, body, opt)) == sorted(_check(tmp_path, body, "-O0"))
    assert len(_check(tmp_path, body, opt)) == 3


# --- switches --------------------------------------------------------------

BAD = "void f(char * owned p)\n{\n  free(p);\n  free(p);\n}\n"


def test_wno_lifetime(tmp_path):
    """The note goes with the warning it belongs to."""
    proc = _compile(tmp_path, BAD, "-Wno-lifetime")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_werror_lifetime(tmp_path):
    proc = _compile(tmp_path, BAD, "-Werror=lifetime")
    assert proc.returncode != 0
    assert [d[:2] for d in _diags(proc)] == [(4, "error"), (3, "note")]


def test_wno_error_lifetime(tmp_path):
    proc = _compile(tmp_path, BAD, "-Werror", "-Wno-error=lifetime")
    assert proc.returncode == 0, proc.stderr
    assert [d[:2] for d in _diags(proc)] == [(4, "warning"), (3, "note")]


def test_not_without_wlifetime(tmp_path):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + BAD)
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-c", str(src), "-o", str(tmp_path / "t.o")], capture_output=True, text=True
    )
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_safe_default_reports(tmp_path):
    """Without -ccompat the dialect and -Wlifetime are on, as errors."""
    src = tmp_path / "t.c"
    src.write_text(PRELUDE.replace("#pragma yasc\n", "") + BAD)
    proc = subprocess.run([str(TCC), "-c", str(src), "-o", str(tmp_path / "t.o")], capture_output=True, text=True)
    assert proc.returncode != 0, proc.stderr
    assert "error: 'p' is freed twice" in proc.stderr


def test_plain_c_is_untouched(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("void free(void *p);\nvoid f(char *owned)\n{\n  free(owned);\n  free(owned);\n}\n")
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-Wlifetime", "-c", str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_code_unchanged(tmp_path, opt):
    corpus = INLINED + BAD
    plain = _compile(tmp_path, corpus, opt, "-Wno-lifetime", obj="plain.o")
    checked = _compile(tmp_path, corpus, opt, obj="checked.o")
    assert plain.returncode == 0 and checked.returncode == 0
    assert (tmp_path / "plain.o").read_bytes() == (tmp_path / "checked.o").read_bytes()


# --- maybe: a pointer that may be null --------------------------------------
#
# A `maybe` variable is followed for null: dereferencing it needs a test on
# every path (`if (p)`, `if (!p) return`, `p != 0 &&`, a noreturn call on the
# null path).  A pointer that is not maybe may never get null: storing a value
# that may be null in one, or returning it from a function whose result is not
# maybe, is reported.  Sources that may be null: maybe variables by their
# state, calls returning maybe, the constant 0.

MAYBE_PRELUDE = """\
struct node { int v; struct node * view maybe next; };
struct node * borrows(static) maybe find(int k);
struct node * borrows(static) must(int k);
_Noreturn void die(void);
"""


def _maybe(tmp_path, body, *flags):
    return [(line - MAYBE_PRELUDE.count("\n"), kind, msg) for line, kind, msg in _check(tmp_path, MAYBE_PRELUDE + body, *flags)]


MAYBE_BAD = {
    "member": ("int f(struct node * maybe p)\n{\n  return p->v;\n}\n", 3, "'p' may be null where it is dereferenced"),
    "index": ("int f(char * maybe s, int i)\n{\n  return s[i];\n}\n", 3, "'s' may be null where it is dereferenced"),
    "store": (
        "void f(int k)\n{\n  struct node * view maybe q = find(k);\n  q->v = 1;\n}\n",
        4,
        "'q' may be null where it is dereferenced",
    ),
    "null": ("int f(void)\n{\n  struct node * view maybe q = 0;\n  return q->v;\n}\n", 4, "'q' is null where it is dereferenced"),
    "tested_on_one_path": (
        "int f(struct node * maybe p, int x)\n{\n  if (x && p)\n    x = 1;\n  return p->v;\n}\n",
        5,
        "'p' may be null where it is dereferenced",
    ),
    "return_maybe": (
        "struct node * borrows(static) f(int k)\n{\n  return find(k);\n}\n",
        3,
        "a pointer that may be null is returned from 'f', whose result is not maybe",
    ),
    "return_null": (
        "struct node * borrows(static) f(void)\n{\n  return 0;\n}\n",
        3,
        "a null pointer is returned from 'f', whose result is not maybe",
    ),
    "store_maybe": (
        "void f(int k)\n{\n  struct node * view q = find(k);\n  (void)q;\n}\n",
        3,
        "a pointer that may be null is stored in 'q', which is not maybe",
    ),
    "store_null": (
        "void f(void)\n{\n  char * view s = 0;\n  (void)s;\n}\n",
        3,
        "a null pointer is stored in 's', which is not maybe",
    ),
    # values followed across blocks: the pointer is read before the branch
    # of `?:` and dereferenced after it, or the arms meet in a temp
    "ternary_untested": ("int f(struct node * maybe p, int c)\n{\n  return c ? p->v : 0;\n}\n", 3, "'p' may be null where it is dereferenced"),
    "store_before_ternary": (
        "void f(inout struct node * maybe p, int c)\n{\n  p->v = c ? 1 : 2;\n}\n",
        3,
        "'p' may be null where it is dereferenced",
    ),
    "ternary_join_stored": (
        "int f(inout struct node * maybe p, int c)\n{\n  struct node * view x = c ? p : must(1);\n  return x->v;\n}\n",
        3,
        "a pointer that may be null is stored in 'x', which is not maybe",
    ),
    "ternary_join_returned": (
        "struct node * borrows(static) f(int c)\n{\n  return c ? find(c) : must(c);\n}\n",
        3,
        "a pointer that may be null is returned from 'f', whose result is not maybe",
    ),
    "switch_value": (
        "int f(struct node * maybe p)\n{\n  switch (p->v)\n  {\n  case 1:\n    return 2;\n  }\n  return 0;\n}\n",
        3,
        "'p' may be null where it is dereferenced",
    ),
}


@pytest.mark.parametrize("name", sorted(MAYBE_BAD))
def test_maybe_reported(tmp_path, name):
    body, line, msg = MAYBE_BAD[name]
    assert _maybe(tmp_path, body) == [(line, "warning", msg)]


MAYBE_CLEAN = {
    "if_set": "int f(struct node * maybe p)\n{\n  if (p)\n    return p->v;\n  return 0;\n}\n",
    "early_return": "int f(struct node * maybe p)\n{\n  if (!p)\n    return 0;\n  return p->v;\n}\n",
    "and_chain": "int f(struct node * maybe p)\n{\n  return p != 0 && p->v;\n}\n",
    "noreturn": "int f(struct node * maybe p)\n{\n  if (!p)\n    die();\n  return p->v;\n}\n",
    "refilled": "int f(int k)\n{\n  struct node * view maybe q = find(k);\n  if (!q)\n    q = must(k);\n"
    "  return q->v;\n}\n",
    "plain_parameter": "int f(struct node *p)\n{\n  return p->v;\n}\n",
    "maybe_result": "struct node * borrows(static) maybe f(int k)\n{\n  return find(k);\n}\n",
    "list_walk": "int f(struct node * maybe p)\n{\n  int n = 0;\n  while (p)\n  {\n    n += p->v;\n"
    "    p = p->next;\n  }\n  return n;\n}\n",
    "tested_return": "struct node * borrows(static) f(int k)\n{\n  struct node * view maybe q = find(k);\n  if (q)\n    return q;\n"
    "  return must(k);\n}\n",
    "ternary": "int f(struct node * maybe p)\n{\n  return p ? p->v : 0;\n}\n",
    "tested_before_ternary": "int f(struct node * maybe p, int c)\n{\n  if (!p)\n    return 0;\n"
    "  return c ? p->v : 0;\n}\n",
    "ternary_join_tested": "int f(inout struct node * maybe p, int c)\n{\n  struct node * view maybe x = c ? p : 0;\n"
    "  return x ? x->v : 0;\n}\n",
    "ternary_join_set": "int f(inout struct node * maybe p, int c)\n{\n  if (!p)\n    return 0;\n"
    "  struct node * view x = c ? p : must(1);\n  return x->v;\n}\n",
}


@pytest.mark.parametrize("name", sorted(MAYBE_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_maybe_clean(tmp_path, name, opt):
    assert _maybe(tmp_path, MAYBE_CLEAN[name], opt) == []


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_maybe_same_at_every_level(tmp_path, opt):
    corpus = "".join(
        body.replace("f(", f"f_{name}(", 1) for name, (body, _, _) in MAYBE_BAD.items()
    )
    assert _maybe(tmp_path, corpus, opt) == _maybe(tmp_path, corpus, "-O0")


# --- out: the function writes the target -------------------------------------
#
# `out T * view p`: *p must be written on every path before the function returns,
# and never read before it is written.  Any write through p counts (a member
# too), and so does handing p on (a call, a store): whoever gets it may write
# the target.  A null `out T * view maybe p` has nothing to write.

OUT_PRELUDE = """\
struct S { int a, b; };
int fill(out struct S *s);
void zero(inout struct S *s);
"""


def _out(tmp_path, body, *flags):
    return [(line - OUT_PRELUDE.count("\n"), kind, msg) for line, kind, msg in _check(tmp_path, OUT_PRELUDE + body, *flags)]


OUT_BAD = {
    "not_on_one_path": (
        "int f(out struct S *s, int x)\n{\n  if (x)\n    return -1;\n  s->a = 1;\n  return 0;\n}\n",
        4,
        "'*s' is not written when 'f' returns",
    ),
    "read_first": ("int f(out int *n)\n{\n  return *n;\n}\n", 3, "'*n' is read before it is written"),
    "never": ("void f(out int *n)\n{\n}\n", 3, "'*n' is not written when 'f' returns"),
    "maybe_read_first": (
        "int f(out int *n, int x)\n{\n  if (x)\n    *n = 1;\n  return *n;\n}\n",
        5,
        "'*n' may be read before it is written",
    ),
    "falls_off_the_end": (
        "void f(out int *n, int x)\n{\n  if (x)\n    *n = 1;\n}\n",
        5,
        # the false edge leaves the function by itself: on that path it is not
        "'*n' is not written when 'f' returns",
    ),
}


@pytest.mark.parametrize("name", sorted(OUT_BAD))
def test_out_reported(tmp_path, name):
    body, line, msg = OUT_BAD[name]
    assert _out(tmp_path, body) == [(line, "warning", msg)]


OUT_CLEAN = {
    "members": "int f(out struct S *s)\n{\n  s->a = 1;\n  s->b = 2;\n  return s->a;\n}\n",
    "whole": "int f(out struct S *s)\n{\n  *s = (struct S){1, 2};\n  return 0;\n}\n",
    "out_callee": "int f(out struct S *s)\n{\n  return fill(s);\n}\n",
    "handed_on": "void f(out struct S *s)\n{\n  zero(s);\n}\n",
    "maybe_null": "void f(out int * maybe n)\n{\n  if (n)\n    *n = 0;\n}\n",
    "every_path": "int f(out int *n, int x)\n{\n  if (x)\n  {\n    *n = 1;\n    return 1;\n  }\n  *n = 0;\n"
    "  return 0;\n}\n",
}


@pytest.mark.parametrize("name", sorted(OUT_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_out_clean(tmp_path, name, opt):
    assert _out(tmp_path, OUT_CLEAN[name], opt) == []


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_out_same_at_every_level(tmp_path, opt):
    corpus = "".join(body.replace("f(", f"f_{name}(", 1) for name, (body, _, _) in OUT_BAD.items())
    assert _out(tmp_path, corpus, opt) == _out(tmp_path, corpus, "-O0")


def test_empty_body_owned_parameter(tmp_path):
    assert _check(tmp_path, "void f(char * owned p)\n{\n}\n") == [
        (3, "warning", "'p' leaks: it still owns memory at the end of the function")
    ]


# --- maybe: arguments ----------------------------------------------------------
#
# A pointer parameter declared in yasc without maybe may never get null: an
# argument that may be null is reported, for a direct call, a call through a
# pointer, and a call the inliner expands (where the argument is stored into
# the callee's parameter rather than passed).  A parameter declared in regular
# C is `view maybe` and takes anything.

ARG_PRELUDE = """\
struct node { int v; };
struct node * borrows(static) maybe find(int k);
int takes(struct node *n);
int takes_maybe(struct node * maybe n);
static int peek(struct node *n) { return n->v; }
int (*takes_fp)(struct node *n);
static int wrap(struct node * maybe q) { return takes(q); }
"""

ARG_BAD = {
    "call_result": ("int f(int k)\n{\n  return takes(find(k));\n}\n", 3, "a pointer that may be null is passed as argument 1 of 'takes', which is not maybe"),
    "null": ("int f(void)\n{\n  return takes(0);\n}\n", 3, "a null pointer is passed as argument 1 of 'takes', which is not maybe"),
    "inlined_callee": (
        "int f(int k)\n{\n  struct node * view maybe q = find(k);\n  return peek(q);\n}\n",
        4,
        "a pointer that may be null is passed as argument 1 of 'peek', which is not maybe",
    ),
    "through_a_pointer": (
        "int f(int k)\n{\n  struct node * view maybe q = find(k);\n  return takes_fp(q);\n}\n",
        4,
        "a pointer that may be null is passed as argument 1, which is not maybe",
    ),
}


def _arg(tmp_path, body, *flags):
    shift = ARG_PRELUDE.count("\n")
    return [(line - shift, kind, msg) for line, kind, msg in _check(tmp_path, ARG_PRELUDE + body, *flags)]


@pytest.mark.parametrize("name", sorted(ARG_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_maybe_argument_reported(tmp_path, name, opt):
    body, line, msg = ARG_BAD[name]
    got = _arg(tmp_path, body, opt)
    # wrap (static, maybe parameter passed on to use) reports its own line once
    assert (line, "warning", msg) in got
    assert [d for d in got if d[0] > 0 and d[0] != line] == []


ARG_CLEAN = {
    "tested": "int f(int k)\n{\n  struct node * view maybe q = find(k);\n  if (q)\n    return takes(q);\n  return 0;\n}\n",
    "maybe_parameter": "int f(int k)\n{\n  return takes_maybe(find(k));\n}\n",
    "tested_inline": "int f(int k)\n{\n  struct node * view maybe q = find(k);\n  return q ? peek(q) : 0;\n}\n",
    "regular_c_parameter": "int legacy(struct node *n);\nint f(int k)\n{\n  return legacy(find(k));\n}\n",
}


@pytest.mark.parametrize("name", sorted(ARG_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_maybe_argument_clean(tmp_path, name, opt):
    body = ARG_CLEAN[name]
    if name == "regular_c_parameter":
        # declared in regular C: a header without #pragma yasc
        (tmp_path / "legacy.h").write_text("struct node;\nint legacy(struct node *n);\n")
        body = body.replace("int legacy(struct node *n);\n", '#include "legacy.h"\n')
    got = [d for d in _arg(tmp_path, body, opt, f"-I{tmp_path}") if d[0] > 0]
    assert got == []


# __attribute__((nonnull)) in a regular-C header is "not maybe" for the flow
# check: a value that may be null is reported as for a yasc parameter.  A
# null constant is -Wnonnull's (reported once, by it).
NONNULL_HEADER = """\
struct node;
int cpy(struct node *d, const struct node *s) __attribute__((nonnull(1, 2)));
int all(struct node *a, struct node *b) __attribute__((nonnull));
int some(struct node *a, struct node *b);
"""

NONNULL_BAD = {
    "call_result": (
        "int f(inout struct node *d, int k)\n{\n  return cpy(d, find(k));\n}\n",
        [(3, "warning", "a pointer that may be null is passed as argument 2 of 'cpy', which is declared nonnull")],
    ),
    "all_parameters": (
        "int f(inout struct node *d, int k)\n{\n  struct node * view maybe q = find(k);\n  return all(d, q);\n}\n",
        [(4, "warning", "a pointer that may be null is passed as argument 2 of 'all', which is declared nonnull")],
    ),
    "null_constant_once": (
        "int f(inout struct node *d)\n{\n  return cpy(d, 0);\n}\n",
        [(3, "warning", "null passed as argument 2, which is declared nonnull")],
    ),
}

NONNULL_CLEAN = {
    "tested": "int f(inout struct node *d, int k)\n{\n  struct node * view maybe q = find(k);\n  return q ? cpy(d, q) : 0;\n}\n",
    "not_nonnull": "int f(int k)\n{\n  return some(find(k), 0);\n}\n",
}


def _nonnull(tmp_path, body, *flags):
    (tmp_path / "nn.h").write_text(NONNULL_HEADER)
    return [d for d in _arg(tmp_path, '#include "nn.h"\n' + body, f"-I{tmp_path}", "-Wnonnull", *flags) if d[0] > 1]


@pytest.mark.parametrize("name", sorted(NONNULL_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_nonnull_attribute_reported(tmp_path, name, opt):
    body, want = NONNULL_BAD[name]
    assert _nonnull(tmp_path, body, opt) == [(l + 1, k, m) for l, k, m in want]


@pytest.mark.parametrize("name", sorted(NONNULL_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_nonnull_attribute_clean(tmp_path, name, opt):
    assert _nonnull(tmp_path, NONNULL_CLEAN[name], opt) == []


def test_wrap_reports_once(tmp_path):
    """The static wrap passes its maybe parameter to use: reported once, at
    its line, at every level (at -O1+ from the end-of-unit check)."""
    for opt in LEVELS:
        got = _arg(tmp_path, "int f(int k)\n{\n  return wrap(find(k));\n}\n", opt)
        assert got == [(0, "warning", "a pointer that may be null is passed as argument 1 of 'takes', which is not maybe")], (opt, got)


# --- borrows: a pointer that owns nothing outliving its owner -----------------
#
# A pointer that owns nothing, assigned from an owned variable (a copy, or into
# what it owns: p + n), borrows from it; used after that owner was freed or
# moved, it is reported, with a note where.  Assigning it again ends the
# borrow.  A pointer borrowing from different owners in different places is
# followed per owner (test_lifetime_paths.py owners_*).

BORROW_PRELUDE = (
    "char * owned make(void);\nchar * borrows(s) maybe find(char *s, int c);\nchar * borrows(s) first(char *s);\n"
)

BORROW_BAD = {
    "freed": (
        "int f(void)\n{\n  char * owned p = make();\n  char * view v = p;\n  free(p);\n  return use(v);\n}\n",
        [(6, "warning", "'v' is used after 'p', which it borrows from, was freed"), (5, "note", "'p' was freed here")],
    ),
    "moved_into_parameter": (
        "int f(void)\n{\n  char * owned p = make();\n  char * view v = p + 2;\n  take(move(p));\n  return *v;\n}\n",
        [(6, "warning", "'v' is used after 'p', which it borrows from, was moved"), (5, "note", "'p' was moved here")],
    ),
    "moved_into_variable": (
        "int f(void)\n{\n  char * owned p = make();\n  char * view v = p;\n  char * owned q = move(p);\n  int r = use(v);\n"
        "  free(q);\n  return r;\n}\n",
        [(6, "warning", "'v' is used after 'p', which it borrows from, was moved"), (5, "note", "'p' was moved here")],
    ),
    "freed_on_one_path": (
        "int f(int c)\n{\n  char * owned p = make();\n  char * view v = p;\n  if (c)\n  {\n    free(p);\n"
        "    p = make();\n  }\n  int r = use(v);\n  free(p);\n  return r;\n}\n",
        [(10, "warning", "'v' may be used after 'p', which it borrows from, was freed"), (7, "note", "'p' was freed here")],
    ),
    # a call whose result borrows(s) from an owned argument
    "through_call": (
        "int f(void)\n{\n  char * owned p = make();\n  char * view maybe v = find(p, 'x');\n  free(p);\n"
        "  return v ? *v : 0;\n}\n",
        [(6, "warning", "'v' is used after 'p', which it borrows from, was freed"), (5, "note", "'p' was freed here")],
    ),
    "through_call_plus_offset": (
        "int f(void)\n{\n  char * owned p = make();\n  char * view v = first(p) + 1;\n  take(move(p));\n  return *v;\n}\n",
        [(6, "warning", "'v' is used after 'p', which it borrows from, was moved"), (5, "note", "'p' was moved here")],
    ),
    # an inlined call (-O1+) borrows what the callee's declared result says
    "through_inline": (
        "static inline char * view next(inout char *s)\n{\n  return s + 1;\n}\n"
        "int f(void)\n{\n  char * owned p = make();\n  char * view v = next(p);\n  free(p);\n  return use(v);\n}\n",
        [(10, "warning", "'v' is used after 'p', which it borrows from, was freed"), (9, "note", "'p' was freed here")],
    ),
    "through_inline_borrows": (
        "static inline char * borrows(s) maybe at(inout char *s, int k)\n{\n  return k ? s + k : 0;\n}\n"
        "int f(int k)\n{\n  char * owned p = make();\n  char * view maybe v = at(p, k);\n  take(move(p));\n"
        "  return v ? *v : 0;\n}\n",
        [(10, "warning", "'v' is used after 'p', which it borrows from, was moved"), (9, "note", "'p' was moved here")],
    ),
    "through_nested_inline": (
        "static inline char * view next(inout char *s)\n{\n  return s + 1;\n}\n"
        "int f(void)\n{\n  char * owned p = make();\n  char * view v = next(next(p));\n  free(p);\n  return *v;\n}\n",
        [(10, "warning", "'v' is used after 'p', which it borrows from, was freed"), (9, "note", "'p' was freed here")],
    ),
}

BORROW_CLEAN = {
    "used_before": "int f(void)\n{\n  char * owned p = make();\n  char * view v = p;\n  int r = use(v);\n  free(p);\n"
    "  return r;\n}\n",
    "borrowed_again": "int f(void)\n{\n  char * owned p = make();\n  char * view v = p;\n  free(p);\n  p = make();\n"
    "  v = p;\n  int r = use(v);\n  free(p);\n  return r;\n}\n",
    "assigned_elsewhere": "int f(inout char *s)\n{\n  char * owned p = make();\n  char * view v = p;\n  free(p);\n"
    "  v = s;\n  return use(v);\n}\n",
    "through_call_used_before": "int f(void)\n{\n  char * owned p = make();\n  char * view v = first(p);\n"
    "  int r = *v;\n  free(p);\n  return r;\n}\n",
    "through_inline_used_before": "static inline char * view next(inout char *s)\n{\n  return s + 1;\n}\n"
    "int f(void)\n{\n  char * owned p = make();\n  char * view v = next(p);\n  int r = *v;\n  free(p);\n"
    "  return r;\n}\n",
    "through_inline_of_a_view": "static inline char * view next(inout char *s)\n{\n  return s + 1;\n}\n"
    "int f(inout char *s)\n{\n  char * owned p = make();\n  char * view v = next(s);\n  free(p);\n  return *v;\n}\n",
    "through_call_of_a_view": "int f(char *s)\n{\n  char * owned p = make();\n  char * view v = first(s);\n"
    "  free(p);\n  return *v;\n}\n",
}


def _borrow(tmp_path, body, *flags):
    shift = BORROW_PRELUDE.count("\n")
    return [(l - shift, k, m) for l, k, m in _check(tmp_path, BORROW_PRELUDE + body, *flags)]


@pytest.mark.parametrize("name", sorted(BORROW_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_borrow_reported(tmp_path, name, opt):
    body, want = BORROW_BAD[name]
    assert _borrow(tmp_path, body, opt) == want


@pytest.mark.parametrize("name", sorted(BORROW_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_borrow_clean(tmp_path, name, opt):
    assert _borrow(tmp_path, BORROW_CLEAN[name], opt) == []


# --- maybe members: `q->next` followed like a maybe variable ------------------
#
# A `T * maybe` member reached through a pointer variable (`q->next`,
# `(*q).next`, and through another such member: `q->next->next`) is followed
# for null per path.  Untested it may be null; `if (q->next)`, `!q->next`,
# `q->next != 0`, `&&`, `?:` and `while (n->next)` refine it; a store sets it
# from the value stored.  Assigning the base again forgets its members, and so
# does a call that gets the base (or `&q->next`) as a pointer to memory that is
# not const; a `const T * view` parameter keeps them.  Stores through other
# pointers are assumed not to alias.

MEMBER_PRELUDE = """\
struct node { int v; struct node * view maybe next; };
struct list { struct list * view maybe next; int v; };
struct node * borrows(static) must(int k);
void update(inout struct node *n);
void show(const struct node *n);
int both(inout struct node *n, int x);
void set_next(inout struct node * view maybe *pp);
int vsum(int n, ...);
static inline void clear_next(inout struct node *n)
{
  n->next = 0;
}
static inline int peek(const struct node *n)
{
  return n->v;
}
"""


def _member(tmp_path, body, *flags):
    shift = MEMBER_PRELUDE.count("\n")
    return [(l - shift, k, m) for l, k, m in _check(tmp_path, MEMBER_PRELUDE + body, *flags)]


MEMBER_BAD = {
    # r may point to the same node: the store forgets q->next too (strict lifetimes)
    "store_elsewhere": (
        "int f(struct node *q, inout struct node *r)\n{\n  if (!q->next)\n    return 0;\n"
        "  r->next = 0;\n  return q->next->v;\n}\n",
        6,
        "'q->next' may be null where it is dereferenced",
    ),
    "untested": ("int f(struct node *q)\n{\n  return q->next->v;\n}\n", 3, "'q->next' may be null where it is dereferenced"),
    "ternary_untested": ("int f(struct node *q, int c)\n{\n  return c ? q->next->v : 0;\n}\n", 3, "'q->next' may be null where it is dereferenced"),
    "copy_then_member_changed": (
        "int f(inout struct node *q)\n{\n  struct node * view maybe n = q->next;\n  q->next = 0;\n  if (n)\n"
        "    return q->next->v;\n  return 0;\n}\n",
        6,
        "'q->next' is null where it is dereferenced",
    ),
    "copy_reassigned": (
        "int f(struct node *q, int c)\n{\n  struct node * view maybe n = q->next;\n  if (c)\n    n = must(1);\n"
        "  if (n)\n    return q->next->v;\n  return 0;\n}\n",
        7,
        "'q->next' may be null where it is dereferenced",
    ),
    "copy_then_call": (
        "int f(inout struct node *q)\n{\n  struct node * view maybe n = q->next;\n  update(q);\n  if (!n)\n"
        "    return 0;\n  return q->next->v;\n}\n",
        7,
        "'q->next' may be null where it is dereferenced",
    ),
    "first_member": ("int f(struct list *q)\n{\n  return q->next->v;\n}\n", 3, "'q->next' may be null where it is dereferenced"),
    "through_dot": ("int f(struct node *q)\n{\n  return (*q).next->v;\n}\n", 3, "'q->next' may be null where it is dereferenced"),
    "store_through": (
        "void f(struct node *q)\n{\n  q->next->v = 1;\n}\n",
        3,
        "'q->next' may be null where it is dereferenced",
    ),
    "stored_null": (
        "int f(inout struct node *q)\n{\n  if (!q->next)\n    return 0;\n  q->next = 0;\n  return q->next->v;\n}\n",
        6,
        "'q->next' is null where it is dereferenced",
    ),
    "tested_on_one_path": (
        "int f(struct node *q, int x)\n{\n  if (x && q->next)\n    x = 1;\n  return q->next->v;\n}\n",
        5,
        "'q->next' may be null where it is dereferenced",
    ),
    "call_forgets": (
        "int f(inout struct node *q)\n{\n  if (!q->next)\n    return 0;\n  update(q);\n  return q->next->v;\n}\n",
        6,
        "'q->next' may be null where it is dereferenced",
    ),
    "varargs_call_forgets": (
        "int f(inout struct node *q)\n{\n  if (!q->next)\n    return 0;\n  vsum(1, q);\n  return q->next->v;\n}\n",
        6,
        "'q->next' may be null where it is dereferenced",
    ),
    "address_passed": (
        "int f(inout struct node *q)\n{\n  if (!q->next)\n    return 0;\n  set_next(&q->next);\n  return q->next->v;\n}\n",
        6,
        "'q->next' may be null where it is dereferenced",
    ),
    "inlined_call_forgets": (
        "int f(inout struct node *q)\n{\n  if (!q->next)\n    return 0;\n  clear_next(q);\n  return q->next->v;\n}\n",
        6,
        "'q->next' may be null where it is dereferenced",
    ),
    "base_assigned": (
        "int f(struct node *q, struct node *r)\n{\n  if (!q->next)\n    return 0;\n  q = r;\n"
        "  return q->next->v;\n}\n",
        6,
        "'q->next' may be null where it is dereferenced",
    ),
    "other_base": (
        "int f(struct node *q, struct node *r)\n{\n  if (q->next)\n    return r->next->v;\n  return 0;\n}\n",
        4,
        "'r->next' may be null where it is dereferenced",
    ),
    "two_levels": (
        "int f(struct node *q)\n{\n  if (!q->next)\n    return 0;\n  return q->next->next->v;\n}\n",
        5,
        "'q->next->next' may be null where it is dereferenced",
    ),
    "returned": (
        "struct node * view f(struct node *q)\n{\n  return q->next;\n}\n",
        3,
        "a pointer that may be null is returned from 'f', whose result is not maybe",
    ),
    "stored_in_plain": (
        "int f(struct node *q)\n{\n  struct node * view n = q->next;\n  return n->v;\n}\n",
        3,
        "a pointer that may be null is stored in 'n', which is not maybe",
    ),
    "reported_once": (
        "int f(struct node *q)\n{\n  return q->next->v + q->next->v;\n}\n",
        3,
        "'q->next' may be null where it is dereferenced",
    ),
}


@pytest.mark.parametrize("name", sorted(MEMBER_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_member_reported(tmp_path, name, opt):
    body, line, msg = MEMBER_BAD[name]
    assert _member(tmp_path, body, opt) == [(line, "warning", msg)]


MEMBER_CLEAN = {
    "if_set": "int f(struct node *q)\n{\n  if (q->next)\n    return q->next->v;\n  return 0;\n}\n",
    "first_member": "int f(struct list *q)\n{\n  if (q->next)\n    return q->next->v;\n  return 0;\n}\n",
    "early_return": "int f(struct node *q)\n{\n  if (!q->next)\n    return 0;\n  return q->next->v;\n}\n",
    "compare_null": "int f(struct node *q)\n{\n  if (q->next != 0)\n    return q->next->v;\n  return 0;\n}\n",
    "and_chain": "int f(struct node *q)\n{\n  return q->next && q->next->v;\n}\n",
    "ternary": "int f(struct node *q)\n{\n  return q->next ? q->next->v : 0;\n}\n",
    "walk_to_last": "int f(struct node *n)\n{\n  while (n->next)\n    n = n->next;\n  return n->v;\n}\n",
    "for_walk": "int f(inout struct node *h)\n{\n  int s = 0;\n  for (struct node * view n = h; n->next; n = n->next)\n"
    "    s += n->next->v;\n  return s;\n}\n",
    "const_view_call": "int f(struct node *q)\n{\n  if (!q->next)\n    return 0;\n  show(q);\n  return q->next->v;\n}\n",
    "inlined_const_view": "int f(struct node *q)\n{\n  if (!q->next)\n    return 0;\n  peek(q);\n"
    "  return q->next->v;\n}\n",
    "argument_read_before_call": "int f(inout struct node *q)\n{\n  if (!q->next)\n    return 0;\n"
    "  return both(q, q->next->v);\n}\n",
    # a local holding a copy of the member: testing one tells about the other
    "copy_tested": "int f(struct node *q)\n{\n  struct node * view maybe n = q->next;\n  if (n)\n"
    "    return q->next->v;\n  return 0;\n}\n",
    "member_tested_copy_used": "int f(struct node *q)\n{\n  struct node * view maybe n = q->next;\n"
    "  if (q->next)\n    return n->v;\n  return 0;\n}\n",
    "copy_early_return": "int f(struct node *q)\n{\n  struct node * view maybe n = q->next;\n  if (!n)\n"
    "    return 0;\n  return q->next->v + n->v;\n}\n",
    "ternary_tested": "int f(struct node *q, int c)\n{\n  if (!q->next)\n    return 0;\n  return c ? q->next->v : 0;\n}\n",
    "stored_set": "int f(inout struct node *q)\n{\n  q->next = must(1);\n  return q->next->v;\n}\n",
    "refilled": "int f(inout struct node *q, int k)\n{\n  if (!q->next)\n    q->next = must(k);\n  return q->next->v;\n}\n",
    "two_levels_tested": "int f(struct node *q)\n{\n  if (q->next && q->next->next)\n    return q->next->next->v;\n"
    "  return 0;\n}\n",
    "maybe_local": "int f(struct node *q)\n{\n  struct node * view maybe n = q->next;\n  if (n)\n    return n->v;\n"
    "  return 0;\n}\n",
    "unlink_next": "void f(inout struct node *q)\n{\n  if (q->next)\n    q->next = q->next->next;\n}\n",
}


@pytest.mark.parametrize("name", sorted(MEMBER_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_member_clean(tmp_path, name, opt):
    assert _member(tmp_path, MEMBER_CLEAN[name], opt) == []


MEMBER_CORPUS = "".join(
    body.replace("f(", f"f_bad_{name}(", 1) for name, (body, _, _) in MEMBER_BAD.items()
) + "".join(body.replace("f(", f"f_ok_{name}(", 1) for name, body in MEMBER_CLEAN.items())


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_member_same_at_every_level(tmp_path, opt):
    assert _member(tmp_path, MEMBER_CORPUS, opt) == _member(tmp_path, MEMBER_CORPUS, "-O0")


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_member_code_unchanged(tmp_path, opt):
    corpus = MEMBER_PRELUDE + MEMBER_CORPUS
    plain = _compile(tmp_path, corpus, opt, "-Wno-lifetime", obj="plain.o")
    checked = _compile(tmp_path, corpus, opt, obj="checked.o")
    assert plain.returncode == 0 and checked.returncode == 0
    assert (tmp_path / "plain.o").read_bytes() == (tmp_path / "checked.o").read_bytes()


# An `owned maybe` member is followed for ownership too: freed (or moved out)
# and then read or freed again is reported, with a note where; a store gives
# it a new value.  It never leaks on its own: its struct owns it.

OWNED_MEMBER_PRELUDE = "struct onode { int v; struct onode * owned maybe next; };\n"

OWNED_MEMBER_BAD = {
    "used_after_free": (
        "int f(inout struct onode *q)\n{\n  if (!q->next)\n    return 0;\n  free(q->next);\n  return q->next->v;\n}\n",
        [(6, "warning", "'q->next' is used after it was freed"), (5, "note", "'q->next' was freed here")],
    ),
    "freed_twice": (
        "void f(inout struct onode *q)\n{\n  free(q->next);\n  free(q->next);\n  q->next = 0;\n}\n",
        [(4, "warning", "'q->next' is freed twice"), (3, "note", "'q->next' was freed here")],
    ),
    "freed_on_one_path": (
        "void f(inout struct onode *q, int c)\n{\n  if (c)\n    free(q->next);\n  free(q->next);\n  q->next = 0;\n}\n",
        [(5, "warning", "'q->next' may be freed twice"), (4, "note", "'q->next' was freed here")],
    ),
    "moved_out": (
        "int f(inout struct onode *q)\n{\n  struct onode * owned maybe n = move(q->next);\n  free(n);\n"
        "  return q->next ? q->next->v : 0;\n}\n",
        [(5, "warning", "'q->next' is used after it was moved"), (3, "note", "'q->next' was moved here")],
    ),
}

OWNED_MEMBER_CLEAN = {
    "freed_then_cleared": "void f(inout struct onode *q)\n{\n  free(q->next);\n  q->next = 0;\n}\n",
    "freed_then_refilled": "int f(inout struct onode *q, struct onode * owned n)\n{\n  free(q->next);\n  q->next = move(n);\n"
    "  return q->next->v;\n}\n",
    "pop_front": "void f(inout struct onode *q)\n{\n  struct onode * owned maybe n = move(q->next);\n  if (n)\n  {\n"
    "    q->next = move(n->next);\n    free(n);\n  }\n}\n",
    "tested_free": "void f(inout struct onode *q)\n{\n  if (q->next)\n    free(q->next);\n  q->next = 0;\n}\n",
}


def _owned_member(tmp_path, body, *flags):
    shift = OWNED_MEMBER_PRELUDE.count("\n")
    return [(l - shift, k, m) for l, k, m in _check(tmp_path, OWNED_MEMBER_PRELUDE + body, *flags)]


@pytest.mark.parametrize("name", sorted(OWNED_MEMBER_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_owned_member_reported(tmp_path, name, opt):
    body, want = OWNED_MEMBER_BAD[name]
    assert _owned_member(tmp_path, body, opt) == want


@pytest.mark.parametrize("name", sorted(OWNED_MEMBER_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_owned_member_clean(tmp_path, name, opt):
    assert _owned_member(tmp_path, OWNED_MEMBER_CLEAN[name], opt) == []


# Maybe members of a base that is not a pointer variable: a local struct
# (`s.next`), a global struct (`g.next`), a global pointer's value
# (`gp->next`).  Followed as `q->next` is: a store sets one, a test refines
# it, passing the struct's address to a parameter that may write it forgets
# it.  A global is assumed unchanged by calls (as stores through other
# pointers are), and is not followed inside an inlined body -- so an inlined
# callee's test or store counts as little as the call's, at every -O.

OBJ_PRELUDE = MEMBER_PRELUDE + """\
struct node * borrows(static) maybe find(int k);
struct node g;
struct node * borrows(static) gp;
static inline int has_next(void)
{
  return g.next != 0;
}
static inline void drop_next(void)
{
  g.next = 0;
}
"""

OBJ_BAD = {
    # a call may change a global's members, an inlined one too (the same at every -O)
    "global_after_call": (
        "int f(inout struct node *q)\n{\n  if (!g.next)\n    return 0;\n  update(q);\n  return g.next->v;\n}\n",
        6,
        "'g.next' may be null where it is dereferenced",
    ),
    "global_after_inlined_store": (
        "int f(void)\n{\n  if (!g.next)\n    return 0;\n  drop_next();\n  return g.next->v;\n}\n",
        6,
        "'g.next' may be null where it is dereferenced",
    ),
    "local_untested": ("int f(int k)\n{\n  struct node s = {0};\n  s.next = find(k);\n  return s.next->v;\n}\n", 5, "'s.next' may be null where it is dereferenced"),
    "local_null_init": ("int f(void)\n{\n  struct node s = {0};\n  return s.next->v;\n}\n", 4, "'s.next' is null where it is dereferenced"),
    "local_passed_to_call": (
        "int f(void)\n{\n  struct node s = undefined;\n  s.next = must(1);\n  update(&s);\n  return s.next->v;\n}\n",
        6,
        "'s.next' may be null where it is dereferenced",
    ),
    "local_across_blocks": (
        "int f(int c, int k)\n{\n  struct node s = undefined;\n  s.next = find(k);\n  return c ? s.next->v : 0;\n}\n",
        5,
        "'s.next' may be null where it is dereferenced",
    ),
    "global_untested": ("int f(void)\n{\n  return g.next->v;\n}\n", 3, "'g.next' may be null where it is dereferenced"),
    "global_null_path": ("int f(void)\n{\n  if (g.next)\n    return 1;\n  return g.next->v;\n}\n", 5, "'g.next' is null where it is dereferenced"),
    "global_pointer": ("int f(void)\n{\n  return gp->next->v;\n}\n", 3, "'gp->next' may be null where it is dereferenced"),
    "global_pointer_reassigned": (
        "int f(void)\n{\n  if (!gp->next)\n    return 0;\n  gp = must(2);\n  return gp->next->v;\n}\n",
        6,
        "'gp->next' may be null where it is dereferenced",
    ),
    "inlined_test_does_not_count": (
        "int f(void)\n{\n  if (has_next())\n    return g.next->v;\n  return 0;\n}\n",
        4,
        "'g.next' may be null where it is dereferenced",
    ),
}

OBJ_CLEAN = {
    "local_tested": "int f(int k)\n{\n  struct node s = undefined;\n  s.next = find(k);\n  if (s.next)\n    return s.next->v;\n  return 0;\n}\n",
    "local_set": "int f(void)\n{\n  struct node s = undefined;\n  s.next = must(1);\n  s.v = 2;\n  return s.next->v;\n}\n",
    "local_const_view_call": "int f(void)\n{\n  struct node s = undefined;\n  s.next = must(1);\n  show(&s);\n  return s.next->v;\n}\n",
    "global_tested": "int f(void)\n{\n  if (!g.next)\n    return 0;\n  return g.next->v;\n}\n",
    "global_pointer_tested": "int f(void)\n{\n  if (gp->next)\n    return gp->next->v;\n  return 0;\n}\n",
}


def _obj(tmp_path, body, *flags):
    shift = OBJ_PRELUDE.count("\n")
    return [(l - shift, k, m) for l, k, m in _check(tmp_path, OBJ_PRELUDE + body, *flags)]


@pytest.mark.parametrize("name", sorted(OBJ_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_object_member_reported(tmp_path, name, opt):
    body, line, msg = OBJ_BAD[name]
    assert _obj(tmp_path, body, opt) == [(line, "warning", msg)]


@pytest.mark.parametrize("name", sorted(OBJ_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_object_member_clean(tmp_path, name, opt):
    assert _obj(tmp_path, OBJ_CLEAN[name], opt) == []


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_object_member_inline_static(tmp_path, opt):
    """A local struct in a static inline function called twice: reported once,
    at its line, at every level."""
    body = (
        "static inline int peek_local(int k)\n{\n  struct node t = undefined;\n  t.next = find(k);\n  return t.next->v;\n}\n"
        "int f(int k)\n{\n  return peek_local(k) + peek_local(k + 1);\n}\n"
    )
    assert _obj(tmp_path, body, opt) == _obj(tmp_path, body, "-O0") == [(5, "warning", "'t.next' may be null where it is dereferenced")]


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_object_member_code_unchanged(tmp_path, opt):
    corpus = OBJ_PRELUDE + "".join(
        body.replace("f(", f"f_bad_{name}(", 1) for name, (body, _, _) in OBJ_BAD.items()
    ) + "".join(body.replace("f(", f"f_ok_{name}(", 1) for name, body in OBJ_CLEAN.items())
    plain = _compile(tmp_path, corpus, opt, "-Wno-lifetime", obj="plain.o")
    checked = _compile(tmp_path, corpus, opt, obj="checked.o")
    assert plain.returncode == 0 and checked.returncode == 0
    assert (tmp_path / "plain.o").read_bytes() == (tmp_path / "checked.o").read_bytes()
