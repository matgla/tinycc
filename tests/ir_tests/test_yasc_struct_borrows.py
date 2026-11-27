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
borrows(static).  The flow check follows a local of such a type: used
after what it borrows was freed or moved, it is reported (flow_* below).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
struct tokenizer borrows(text) { const char *borrows(text) pos; int n; };
struct tokenizer borrows(src) tok_init(const char *src);
struct tokenizer tok_from(const char *src, int k);
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
    body = "void f(char *s)\n{\n  struct tokenizer t = tok_init(s);\n  t.pos = s + 1;\n  (void)t;\n}\n"
    assert _diags(tmp_path, body, opt) == []


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_static_value_into_a_global(tmp_path, opt):
    assert _diags(tmp_path, "void f(void)\n{\n  g_tok = tok_static();\n}\n", opt) == []


STORES = {
    "global": "void f(char *s)\n{\n  g_tok = tok_init(s);\n}\n",
    "through_a_pointer": "void f(inout struct tokenizer *o, const char *s)\n{\n  *o = tok_from(s, 1);\n}\n",
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
    "unknown_parameter": ("struct tokenizer borrows(x) h(const char *s);", "borrows(x): no parameter is called 'x'"),
    "not_borrowing": ("struct other { int a; };\nstruct other borrows(static) z;", "borrows(...): 'struct other' does not borrow"),
    "definition_needs_names": ("struct bad borrows(static) { int a; };", "a struct borrows from names it is given: borrows(text)"),
}


@pytest.mark.parametrize("name", sorted(ERRORS))
def test_errors(tmp_path, name):
    decl, msg = ERRORS[name]
    proc = _compile(tmp_path, decl + "\n")
    assert proc.returncode != 0
    assert [l.split("error: ", 1)[1] for l in proc.stderr.splitlines() if "error: " in l][0] == msg


# --- the flow check: a local of a borrowing struct after its source is gone ---
#
# A local initialised or assigned from a call whose struct result borrows
# (`struct T borrows(src)`) borrows the owned variable passed there; a copy of
# such a local borrows the same; a pointer stored into a pointer member
# borrows what it points into.  A pointer member read after that owner was
# freed or moved is reported -- at every -O: an inlined call's result
# borrows what its type says, whatever its body does, and so does a small
# struct returned in registers.

FLOW_PRELUDE = """\
void free(drop void *?owned p);
void take(char *owned p);
char *owned make(void);
struct tokenizer borrows(a, b) tok2(const char *a, const char *b);
int tok_next(inout struct tokenizer *t);
int tok_peek(const struct tokenizer *t);
struct span borrows(t) { const char *borrows(t) p; };
struct span borrows(src) span_of(const char *src);
static inline struct tokenizer borrows(src) tok_lit(const char *src)
{
  struct tokenizer t = undefined;
  t.pos = "x";
  t.n = *src;
  return t;
}
static inline struct tokenizer borrows(src) tok_wrap(const char *src, int c)
{
  struct tokenizer t = tok_init(src);
  if (c)
    t.n = 1;
  return t;
}
"""
FLOW_LINES = FLOW_PRELUDE.count("\n")
LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _flow(tmp_path, body, opt):
    proc = _compile(tmp_path, FLOW_PRELUDE + body, opt)
    assert proc.returncode == 0, proc.stderr
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "note"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES - FLOW_LINES, parts[1], parts[2]))
    return out


def _uaf(line, name, owner, at, how="freed", is_="is", borrows="borrows"):
    return [
        (line, "warning", f"'{name}' {is_} used after '{owner}', which it {borrows} from, was {how}"),
        (at, "note", f"'{owner}' was {how} here"),
    ]


FLOW_BAD = {
    "initialised": (
        "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(b);\n  free(b);\n  return *t.pos;\n}\n",
        _uaf(6, "t", "b", 5),
    ),
    "assigned": (
        "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = undefined;\n  t = tok_from(b, 1);\n  free(b);\n"
        "  return *t.pos;\n}\n",
        _uaf(7, "t", "b", 6),
    ),
    "copied": (
        "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(b);\n  struct tokenizer u = t;\n"
        "  free(b);\n  return *u.pos + t.n;\n}\n",
        _uaf(7, "u", "b", 6),
    ),
    "moved": (
        "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(b);\n  take(move(b));\n"
        "  return *t.pos;\n}\n",
        _uaf(6, "t", "b", 5, how="moved"),
    ),
    "read_through_a_view": (
        "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(b);\n  free(b);\n"
        "  return tok_peek(&t);\n}\n",
        _uaf(6, "t", "b", 5),
    ),
    "two_sources": (
        "int f(void)\n{\n  char *owned a = make();\n  char *owned b = make();\n  struct tokenizer t = tok2(a, b);\n"
        "  free(a);\n  int r = *t.pos;\n  free(b);\n  return r;\n}\n",
        _uaf(7, "t", "a", 6),
    ),
    "freed_on_one_path": (
        "int f(int c)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(b);\n  if (c)\n  {\n    free(b);\n"
        "    b = make();\n  }\n  int r = *t.pos;\n  free(b);\n  return r;\n}\n",
        _uaf(10, "t", "b", 7, is_="may be"),
    ),
    "member_stored": (
        "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_static();\n  t.pos = b + 1;\n  free(b);\n"
        "  return *t.pos;\n}\n",
        _uaf(7, "t", "b", 6),
    ),
    # the inlined body stores a literal: the call's type still says src
    "inlined_body_says_otherwise": (
        "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_lit(b);\n  free(b);\n  return *t.pos;\n}\n",
        _uaf(6, "t", "b", 5),
    ),
    # a static callee (in registers at -O1, inlined at -O2)
    "static_callee": (
        "int f(int c)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_wrap(b, c);\n  free(b);\n"
        "  return *t.pos;\n}\n",
        _uaf(6, "t", "b", 5),
    ),
    # four bytes: returned in r0
    "in_a_register": (
        "int f(void)\n{\n  char *owned b = make();\n  struct span s = span_of(b);\n  free(b);\n  return *s.p;\n}\n",
        _uaf(6, "s", "b", 5),
    ),
}

FLOW_CLEAN = {
    "used_before": "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(b);\n  int r = *t.pos;\n"
    "  free(b);\n  return r;\n}\n",
    "borrowed_again": "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(b);\n  free(b);\n"
    "  b = make();\n  t = tok_init(b);\n  int r = *t.pos;\n  free(b);\n  return r;\n}\n",
    "member_repointed": "int f(char *s)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(b);\n"
    "  free(b);\n  t.pos = s;\n  return *t.pos;\n}\n",
    "other_member_read": "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(b);\n  free(b);\n"
    "  return t.n;\n}\n",
    # a callee that may write the struct: it forgets what it borrowed
    "written_by_a_call": "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(b);\n"
    "  tok_next(&t);\n  free(b);\n  return *t.pos;\n}\n",
    # its address kept: not followed
    "address_kept": "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(b);\n"
    "  struct tokenizer *view tp = &t;\n  free(b);\n  return *tp->pos;\n}\n",
    "static_source": "int f(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_static();\n  free(b);\n"
    "  return *t.pos;\n}\n",
    "in_a_loop": "int f(int n)\n{\n  char *owned b = make();\n  int r = 0;\n  for (int i = 0; i < n; i++)\n  {\n"
    "    struct tokenizer t = tok_init(b);\n    r += *t.pos;\n  }\n  free(b);\n  return r;\n}\n",
    "view_source": "int f(char *s)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(s);\n"
    "  free(b);\n  return *t.pos;\n}\n",
}


@pytest.mark.parametrize("name", sorted(FLOW_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_flow_reported(tmp_path, name, opt):
    body, want = FLOW_BAD[name]
    assert _flow(tmp_path, body, opt) == want


@pytest.mark.parametrize("name", sorted(FLOW_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_flow_clean(tmp_path, name, opt):
    assert _flow(tmp_path, FLOW_CLEAN[name], opt) == []


def test_flow_inline_twice_once(tmp_path):
    """In a static inline called twice: reported once, at every -O."""
    body = (
        "static inline int two(void)\n{\n  char *owned b = make();\n  struct tokenizer t = tok_init(b);\n  free(b);\n"
        "  return *t.pos;\n}\nint f(void)\n{\n  return two() + two();\n}\n"
    )
    for opt in LEVELS:
        assert _flow(tmp_path, body, opt) == _uaf(6, "t", "b", 5), opt


def test_flow_wno_lifetime(tmp_path):
    body, _ = FLOW_BAD["initialised"]
    proc = _compile(tmp_path, FLOW_PRELUDE + body, "-Wno-lifetime")
    assert proc.returncode == 0 and "warning" not in proc.stderr, proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_flow_code_unchanged(tmp_path, opt):
    corpus = FLOW_PRELUDE + "".join(
        body.replace("int f(", f"int f{i}(", 1)
        for i, body in enumerate([b for b, _ in FLOW_BAD.values()] + list(FLOW_CLEAN.values()))
    )
    objs = []
    for flags in (["-Wno-lifetime"], []):
        src = tmp_path / "c.c"
        src.write_text(PRELUDE + corpus)
        out = tmp_path / f"c{len(objs)}.o"
        proc = subprocess.run(
            [str(TCC), "-ccompat", "-Wlifetime", *flags, "-c", opt, str(src), "-o", str(out)],
            capture_output=True,
            text=True,
        )
        assert proc.returncode == 0, proc.stderr
        objs.append(out.read_bytes())
    assert objs[0] == objs[1]
