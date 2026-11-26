"""yasc: a view does not escape; borrows(...) says where a pointer points
(docs/static_analysis_todo.md, "`view` does not escape").

A `view` parameter is borrowed for the call: passing it down is fine, keeping
it is not.  -Wlifetime follows the values that may point into what a view
parameter points at (the parameter, its copies, p + n, `c ? a : b`, a pointer
read through it (`n->next`: the IR marks the loads that give a pointer), and
the result of a call whose result borrows from it) and reports one that is
returned from a function whose result does not borrow from that parameter,
or stored into a global or through a parameter: "'s' is a view: it cannot
leave the function".

A function's view result says its sources: `borrows(s)`, `borrows(a, b)`,
`borrows(static)`, `borrows(static, s)`; written `view`, it borrows from
every view parameter (the elision), and with none it is an error that asks
for borrows(static).  An owned parameter is never a source.  The checks
are the same at every -O level and never change the code.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
char * borrows(s) maybe find(char *s, int c);
const char * borrows(static) name(int e);
char * view g;
const char * view gc;
struct S { char * view f; };
struct node { int len; struct node * view next; char * view name; };
struct node * view gn;
int gi;
"""
PRELUDE_LINES = PRELUDE.count("\n")
LEVELS = ["-O0", "-O1", "-O2", "-Os"]


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
        if len(parts) == 3 and parts[1] in ("warning", "error", "note"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[1], parts[2]))
    return out


def _error(tmp_path, body):
    proc = _compile(tmp_path, body)
    assert proc.returncode != 0, "expected a compile error"
    return [line.split("error: ", 1)[1] for line in proc.stderr.splitlines() if "error: " in line][0]


def _escape(name, how):
    return f"'{name}' is a view: it cannot leave the function ({how})"


RETURNED = "it is returned, and the result does not borrow from it"

ESCAPES = {
    "returned_other_parameter": ("char * borrows(a) f(char *a, inout char *b)\n{\n  return b;\n}\n", 3, "b", RETURNED),
    "returned_as_static": ("const char * borrows(static) f(char *s)\n{\n  return s + 1;\n}\n", 3, "s", RETURNED),
    "stored_in_global": ("void f(inout char *s)\n{\n  g = s;\n}\n", 3, "s", "it is stored in global 'g'"),
    "stored_through_parameter": (
        "void f(inout struct S *o, inout char *s)\n{\n  o->f = s;\n}\n",
        3,
        "s",
        "it is stored through 'o'",
    ),
    "stored_through_out": (
        "void f(inout char * view *o, inout char *s)\n{\n  *o = s;\n}\n",
        3,
        "s",
        "it is stored through 'o'",
    ),
    "through_a_local": (
        "void f(inout char *s)\n{\n  char * view q = s + 2;\n  char * view r = q;\n  g = r;\n}\n",
        5,
        "s",
        "it is stored in global 'g'",
    ),
    "through_a_loop": (
        "char * borrows(a) f(inout char *a, inout char *b, int n)\n{\n  char * view p = a;\n  while (n--)\n"
        "    p = b;\n  return p;\n}\n",
        6,
        "b",
        RETURNED,
    ),
    "through_a_call": ("void f(char *s)\n{\n  g = find(s, 1);\n}\n", 3, "s", "it is stored in global 'g'"),
    "loaded_pointer": ("void f(struct node *n)\n{\n  gn = n->next;\n}\n", 3, "n", "it is stored in global 'gn'"),
    "loaded_member_returned": (
        "const char * borrows(static) f(struct node *n)\n{\n  return n->name;\n}\n",
        3,
        "n",
        RETURNED,
    ),
    "through_an_owned_parameter": (
        "struct T { char * view f; };\nvoid f(struct T * owned o, inout char *s)\n{\n  o->f = s;\n  (void)o;\n}\n",
        4,
        "s",
        "it is stored through a parameter",
    ),
    "call_result_returned": (
        "const char * borrows(static) f(char *s)\n{\n  return find(s + 1, 2) ? s : name(0);\n}\n",
        3,
        "s",
        RETURNED,
    ),
    "conditional": (
        "char * borrows(a) f(inout char *a, char *b)\n{\n  char * view maybe p = find(b, 3);\n"
        "  return p ? p : a;\n}\n",
        4,
        "b",
        RETURNED,
    ),
}


@pytest.mark.parametrize("name", sorted(ESCAPES))
@pytest.mark.parametrize("opt", LEVELS)
def test_escapes(tmp_path, name, opt):
    body, line, who, how = ESCAPES[name]
    assert (line, "warning", _escape(who, how)) in _diags(tmp_path, body, opt)


CLEAN = {
    "elided_one": "char * view f(inout char *s)\n{\n  return s + 1;\n}\n",
    "elided_several": "char * view f(inout char *a, inout char *b, int c)\n{\n  return c ? a : b;\n}\n",
    "named_source": "char * borrows(b) f(char *a, inout char *b)\n{\n  (void)a;\n  return b;\n}\n",
    "static_literal": "const char * borrows(static) f(int e)\n{\n  return e ? \"a\" : \"b\";\n}\n",
    "static_and_parameter": "char * borrows(static, s) maybe f(inout char *s, int e)\n{\n  return e ? s : 0;\n}\n",
    "static_call": "void f(int e)\n{\n  gc = name(e);\n}\n",
    "call_result_returned": "char * view maybe f(char *s)\n{\n  return find(s, 'x');\n}\n",
    "local_struct": "void f(inout char *s)\n{\n  struct S t = undefined;\n  t.f = s;\n  (void)t;\n}\n",
    "passed_down": "int use(char *p);\nint f(char *s)\n{\n  return use(s + 1);\n}\n",
    "local_copy": "int f(inout char *s)\n{\n  char * view q = s;\n  return q[0];\n}\n",
    "read_through": "char f(char *s)\n{\n  g = 0;\n  return *s;\n}\n",
    "within_the_same_object": "void f(inout struct node *n)\n{\n  n->next = n->next->next;\n}\n",
    "loaded_int": "void f(struct node *n)\n{\n  gi = n->len;\n}\n",
    "loaded_pointer_returned": "struct node * view f(struct node *n)\n{\n  return n->next->next;\n}\n",
    "loaded_pointer_local": "void f(struct node *n)\n{\n  struct node * view m = n->next;\n  if (m)\n"
    "    gi = m->len;\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_clean(tmp_path, name, opt):
    assert _diags(tmp_path, CLEAN[name], opt) == []


DECL_ERRORS = {
    "no_source": ("char * view f(void);", "a view result needs a source: write borrows(static), or take a view parameter"),
    "unknown_name": ("char * borrows(x) f(char *s);", "borrows(x): no parameter is called 'x'"),
    "not_a_pointer": ("char * borrows(n) f(char *s, int n);", "borrows(n): 'n' is not a pointer"),
    "owned_source": (
        "char * borrows(s) f(char * owned s);",
        "borrows(s): an owned parameter is consumed, it is never a source",
    ),
    "with_owned": ("char * owned borrows(s) f(char *s);", "a pointer is owned, a view or borrows(...): one of them"),
    "with_view": ("char * view borrows(s) f(char *s);", "a pointer is owned, a view or borrows(...): one of them"),
    "names_on_a_variable": ("char * borrows(s) v;", "borrows(s): only a function's result or a struct member names its sources"),
    "inner_pointer": ("char * borrows(static) * f(void);", "borrows(...) qualifies the outermost pointer"),
}


@pytest.mark.parametrize("name", sorted(DECL_ERRORS))
def test_declaration_errors(tmp_path, name):
    decl, msg = DECL_ERRORS[name]
    assert _error(tmp_path, decl + "\n") == msg


def test_declarations_accepted(tmp_path):
    body = (
        "char * borrows(static) maybe t1(int k);\n"
        "char * borrows(static, s) t2(char *s);\n"
        "char * maybe borrows(s) t3(char *s);\n"
        "const char * borrows(static) lit = \"x\";\n"
        "char * view (*fp)(char *s);\n"
        "typedef char * borrows(static) (*fn_t)(int);\n"
    )
    assert _diags(tmp_path, body) == []


def test_regular_c_is_untouched(tmp_path):
    src = tmp_path / "c.c"
    src.write_text("char *borrows(int x);\nchar *g;\nvoid f(char *s) { g = s; }\nchar *h(void) { return 0; }\n")
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-Wall", "-c", str(src), "-o", str(tmp_path / "c.o")], capture_output=True, text=True
    )
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_code_unchanged(tmp_path, opt):
    """borrows(...) never changes the generated code."""
    body = "char * borrows(s) f(inout char *s, int k)\n{\n  return s + k;\n}\n"
    a = _compile(tmp_path, body, opt)
    assert a.returncode == 0, a.stderr
    first = (tmp_path / "t.o").read_bytes()
    b = _compile(tmp_path, body.replace("borrows(s)", "view"), opt)
    assert b.returncode == 0, b.stderr
    assert (tmp_path / "t.o").read_bytes() == first


# --- a view of an owned variable ---------------------------------------------
# A view of an owned local (a copy in a view, p + n, p->x) stored where it
# outlives the function, while the variable itself is freed or dropped:
# "a view of 'p' is stored in global 'g', which outlives it".  Fine when
# the owner moves somewhere lasting too: into an owned global or member, an
# owned parameter that is not drop, or the owned result.

OWNED_PRELUDE = """\
void * owned malloc(unsigned n);
void free(drop void * owned maybe p);
void keep(char * owned p);
struct box { char * owned buf; char * view key; };
char * owned go;
"""

OWNED_BAD = {
    "freed": ("void f(void)\n{\n  char * owned p = malloc(4);\n  g = p + 1;\n  free(p);\n}\n", 4,
              "a view of 'p' is stored in global 'g', which outlives it"),
    "through_a_view_copy": (
        "void f(void)\n{\n  char * owned p = malloc(4);\n  char * view v = p;\n  g = v;\n  free(p);\n}\n",
        5,
        "a view of 'p' is stored in global 'g', which outlives it",
    ),
    "through_a_parameter": (
        "void f(inout struct box *b)\n{\n  char * owned p = malloc(4);\n  b->key = p;\n  free(p);\n}\n",
        4,
        "a view of 'p' is stored through 'b', which outlives it",
    ),
}

OWNED_CLEAN = {
    "moved_to_a_global": "void f(void)\n{\n  char * owned p = malloc(4);\n  g = p;\n  go = move(p);\n}\n",
    "moved_to_a_member": "void f(inout struct box *b)\n{\n  char * owned p = malloc(4);\n  b->key = p;\n"
    "  free(b->buf);\n  b->buf = move(p);\n}\n",
    "returned": "char * owned f(void)\n{\n  char * owned p = malloc(4);\n  g = p;\n  return p;\n}\n",
    "handed_on": "void f(void)\n{\n  char * owned p = malloc(4);\n  g = p;\n  keep(move(p));\n}\n",
    "into_itself": "struct box * owned f(void)\n{\n  struct box * owned n = malloc(sizeof *n);\n"
    "  n->buf = malloc(4);\n  n->key = n->buf;\n  return n;\n}\n",
}


@pytest.mark.parametrize("name", sorted(OWNED_BAD))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_owned_view_escapes(tmp_path, name, opt):
    body, line, msg = OWNED_BAD[name]
    shift = OWNED_PRELUDE.count("\n")
    diags = [(l - shift, k, m) for l, k, m in _diags(tmp_path, OWNED_PRELUDE + body, opt)]
    assert (line, "warning", msg) in diags


@pytest.mark.parametrize("name", sorted(OWNED_CLEAN))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_owned_view_clean(tmp_path, name, opt):
    assert _diags(tmp_path, OWNED_PRELUDE + OWNED_CLEAN[name], opt) == []


# --- borrows(member) members --------------------------------------------------
# `char * borrows(buf) key` in a struct: what is stored in s->key points into
# the same object's buf (what was loaded from a pointer member, anywhere in
# an array member), or is null or static storage.

FIELD_PRELUDE = """\
struct cache { char * owned buf; int n; char * borrows(buf) key; char name[8];
               const char * borrows(name, static) label; };
"""
KEY = "'key' borrows from 'buf': it can hold only a pointer into the same object's 'buf'"
LABEL = "'label' borrows from 'name': it can hold only a pointer into the same object's 'name'"

FIELD_BAD = {
    "other_object": ("void f(inout struct cache *c, struct cache *d)\n{\n  c->key = d->buf;\n}\n", 3, KEY),
    "a_parameter": ("void f(inout struct cache *c, inout char *s)\n{\n  c->key = s;\n}\n", 3, KEY),
    "other_member": ("void f(inout struct cache *c)\n{\n  c->label = c->buf;\n}\n", 3, LABEL),
    "through_a_local": (
        "void f(inout struct cache *c, struct cache *d)\n{\n  char * view k = d->buf;\n  c->key = k;\n}\n",
        4,
        KEY,
    ),
    "local_from_other_member": (
        "void f(inout struct cache *c)\n{\n  const char * view k = c->buf;\n  c->label = k;\n}\n",
        4,
        LABEL,
    ),
}

FIELD_CLEAN = {
    "pointer_member": "void f(inout struct cache *c)\n{\n  c->key = c->buf + 2;\n}\n",
    "borrow": "void f(inout struct cache *c)\n{\n  c->key = borrow(c->buf);\n}\n",
    "indexed": "void f(inout struct cache *c, int i)\n{\n  c->key = c->buf + i;\n  c->label = c->name + i;\n}\n",
    "array_member": "void f(inout struct cache *c)\n{\n  c->label = c->name + 1;\n}\n",
    "null_and_static": "void f(inout struct cache *c)\n{\n  c->key = 0;\n  c->label = \"x\";\n}\n",
    "local_object": "void f(struct cache c)\n{\n  c.key = c.buf;\n  (void)c;\n}\n",
    "through_a_local": "void f(inout struct cache *c)\n{\n  char * view k = c->buf + 2;\n  c->key = k;\n}\n",
    "through_a_local_on_two_paths": "void f(inout struct cache *c, int x)\n{\n  char * view k = c->buf;\n"
    "  if (x)\n    k = k + 1;\n  c->key = k;\n}\n",
    "local_into_array": "void f(inout struct cache *c)\n{\n  const char * view k = c->name + 3;\n  c->label = k;\n}\n",
    # paths that disagree: not known, not reported
    "local_from_either": "void f(inout struct cache *c, struct cache *d, int x)\n{\n  char * view k = c->buf;\n"
    "  if (x)\n    k = d->buf;\n  c->key = k;\n}\n",
}


@pytest.mark.parametrize("name", sorted(FIELD_BAD))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_field_borrows_reported(tmp_path, name, opt):
    body, line, msg = FIELD_BAD[name]
    shift = FIELD_PRELUDE.count("\n")
    diags = [(l - shift, k, m) for l, k, m in _diags(tmp_path, FIELD_PRELUDE + body, opt)]
    assert (line, "warning", msg) in diags


@pytest.mark.parametrize("name", sorted(FIELD_CLEAN))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_field_borrows_clean(tmp_path, name, opt):
    assert _diags(tmp_path, FIELD_PRELUDE + FIELD_CLEAN[name], opt) == []


FIELD_DECL_ERRORS = {
    "unknown": ("struct a { char * borrows(zz) k; };", "borrows(zz): no member is called 'zz'"),
    "itself": ("struct b { char * borrows(k) k; };", "borrows(k): a member cannot borrow from itself"),
    "not_a_pointer": ("struct c { int n; char * borrows(n) k; };", "borrows(n): 'n' is neither a pointer nor an array"),
}


@pytest.mark.parametrize("name", sorted(FIELD_DECL_ERRORS))
def test_field_borrows_declaration_errors(tmp_path, name):
    decl, msg = FIELD_DECL_ERRORS[name]
    assert _error(tmp_path, decl + "\n") == msg


def test_a_method_named_borrows(tmp_path):
    body = "struct m { int v;\n  int * view borrows(inout struct m *self) { return &self->v; } };\n"
    assert _diags(tmp_path, body) == []


# a borrows(member) member read after its source member was freed or changed
KILL_PRELUDE = """\
void free(drop void * owned maybe p);
char * owned maybe dup(const char *s);
int use(char * maybe p);
struct kc { char * owned maybe buf; char * borrows(buf) maybe key; };
"""
DEAD = "'key' may be used after 'buf', which it borrows from, was freed or changed"

KILL_BAD = {
    "freed": ("void f(inout struct kc *c)\n{\n  free(c->buf);\n  use(c->key);\n}\n", 4),
    "freed_on_one_path": ("void f(inout struct kc *c, int x)\n{\n  if (x)\n    free(c->buf);\n  use(c->key);\n}\n", 5),
    "replaced": ("void f(inout struct kc *c)\n{\n  free(c->buf);\n  c->buf = dup(\"x\");\n  use(c->key);\n}\n", 5),
}
KILL_CLEAN = {
    "key_set_again": "void f(inout struct kc *c)\n{\n  free(c->buf);\n  c->key = 0;\n  use(c->key);\n}\n",
    "read_before": "void f(inout struct kc *c)\n{\n  use(c->key);\n  free(c->buf);\n}\n",
    "other_object": "void f(struct kc *c, inout struct kc *d)\n{\n  free(d->buf);\n  d->buf = 0;\n  use(c->key);\n}\n",
    "pointed_again": "void f(inout struct kc *c)\n{\n  free(c->buf);\n  c->buf = dup(\"x\");\n  c->key = c->buf;\n"
    "  use(c->key + 1);\n}\n",
}


@pytest.mark.parametrize("name", sorted(KILL_BAD))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_field_borrow_dead(tmp_path, name, opt):
    body, line = KILL_BAD[name]
    shift = KILL_PRELUDE.count("\n")
    diags = [(l - shift, k, m) for l, k, m in _diags(tmp_path, KILL_PRELUDE + body, opt)]
    assert (line, "warning", DEAD) in diags


@pytest.mark.parametrize("name", sorted(KILL_CLEAN))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_field_borrow_alive(tmp_path, name, opt):
    assert [d for d in _diags(tmp_path, KILL_PRELUDE + KILL_CLEAN[name], opt) if "borrows" in d[2]] == []
