"""yasc tagged unions (S2 U3), docs/static_analysis_todo.md.

    struct value {
      enum vkind k;
      union(k) { long i when(V_INT); char * view s when(V_STR); } u;
    };

A member of u is used (read, written, its address taken) only where k is
known to select it on every path: k compared with a constant (if, &&, ?:,
switch with case ranges, an early return) or a constant stored in it.  A
store of anything else to k, assigning the base again, or handing the base
to a call that may write through it (not `const T * view`) forgets what was
known; any call forgets a global's.  -Wlifetime (strict): unsafe lowers it.
The same at every -O.  A tagged member is not a U2 union read: a view in it
is a view, not regular C.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

PRELUDE = """\
enum vkind { V_INT, V_STR, V_ADDR, V_D, V_E };
int use(char *p);
struct value;
int look(const struct value *v);
void poke(inout struct value *v);
struct value {
  enum vkind k;
  union(k) {
    long i when(V_INT);
    char * view s when(V_STR);
    unsigned long a when(V_ADDR);
    int de when(V_D, V_E);
    struct { char * view path; } st when(V_ADDR);
    char * view arr[2] when(V_E);
  } u;
};
struct value g;
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _compile(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-sign-conversion", "-Wno-unused-parameter", "-c", "-fmax-errors=0", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            where = parts[0].rsplit(":", 1)
            n = int(where[1]) - PRELUDE_LINES if len(where) == 2 and where[1].isdigit() else 0
            out.append((n, parts[1], parts[2]))
    return out


def _msg(member, values):
    return f"'u.{member}' is used where 'k' may not be {values}: test it first"


GOOD = {
    "switch": (
        "int f(struct value *v)\n{\n  switch (v->k) {\n  case V_INT: return cast<int(int)>(v->u.i);\n"
        "  case V_STR: return use(v->u.s);\n  case V_D: case V_E: return v->u.de;\n  default: return 0;\n  }\n}\n"
    ),
    "early_return": "int f(struct value *v)\n{\n  if (v->k != V_STR)\n    return 0;\n  return use(v->u.s);\n}\n",
    "and": "int f(struct value *v)\n{\n  return v->k == V_STR && use(v->u.s);\n}\n",
    "ternary": "int f(struct value *v)\n{\n  return v->k == V_STR ? use(v->u.s) : 0;\n}\n",
    "store_tag_first": "void f(inout struct value *v)\n{\n  v->k = V_INT;\n  v->u.i = 3;\n}\n",
    "loop": (
        "int f(inout struct value *v)\n{\n  int n = 0;\n  while (v->k == V_STR) {\n    n += use(v->u.s);\n"
        "    v->k = V_INT;\n  }\n  return n;\n}\n"
    ),
    "const_call_keeps": (
        "int f(struct value *v)\n{\n  if (v->k != V_STR)\n    return 0;\n  look(v);\n  return use(v->u.s);\n}\n"
    ),
    "nested_struct": "int f(struct value *v)\n{\n  if (v->k == V_ADDR)\n    return use(v->u.st.path);\n  return 0;\n}\n",
    "array": "int f(struct value *v)\n{\n  if (v->k == V_E)\n    return use(v->u.arr[1]);\n  return 0;\n}\n",
    "test_zero": "long f(struct value *v)\n{\n  if (!v->k)\n    return v->u.i;\n  return 0;\n}\n",
    "local": "long f(void)\n{\n  struct value v = undefined;\n  v.k = V_INT;\n  v.u.i = 3;\n  return v.u.i;\n}\n",
    "global": "unsigned long f(void)\n{\n  if (g.k == V_ADDR)\n    return g.u.a;\n  return 0;\n}\n",
    "unsafe": "int f(struct value *v)\n{\n  return use(unsafe(v->u.s));\n}\n",
    "tag_copied": (
        "int f(struct value *v)\n{\n  enum vkind k = v->k;\n  if (k != V_STR)\n    return 0;\n  return use(v->u.s);\n}\n"
    ),
    "switch_on_copy": (
        "int f(struct value *v)\n{\n  enum vkind k = v->k;\n  switch (k) {\n  case V_INT: return cast<int(int)>(v->u.i);\n"
        "  case V_STR: return use(v->u.s);\n  default: return 0;\n  }\n}\n"
    ),
    "global_pointer": "struct value * view gp;\nlong f(void)\n{\n  if (gp->k == V_INT)\n    return gp->u.i;\n  return 0;\n}\n",
    "jump_to_the_end": "void f(struct value *v)\n{\n  if (v->k == V_STR)\n    use(v->u.s);\n}\n",
    "inline": (
        "static inline long peek(struct value *v)\n{\n  return v->k == V_INT ? v->u.i : -1;\n}\n"
        "long f(struct value *v)\n{\n  return peek(v) + peek(v);\n}\n"
    ),
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(GOOD))
def test_tag_known(tmp_path, name, opt):
    proc = _compile(tmp_path, GOOD[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


BAD = {
    "untested": ("int f(struct value *v)\n{\n  return use(v->u.s);\n}\n", 3, _msg("s", "V_STR")),
    "wrong_variant": (
        "int f(struct value *v)\n{\n  if (v->k == V_INT)\n    return use(v->u.s);\n  return 0;\n}\n",
        4,
        _msg("s", "V_STR"),
    ),
    "member_before_tag": (
        "void f(inout struct value *v)\n{\n  v->u.i = 3;\n  v->k = V_INT;\n}\n",
        3,
        _msg("i", "V_INT"),
    ),
    "call_forgets": (
        "int f(inout struct value *v)\n{\n  if (v->k != V_STR)\n    return 0;\n  poke(v);\n  return use(v->u.s);\n}\n",
        6,
        _msg("s", "V_STR"),
    ),
    "global_call_forgets": (
        "unsigned long f(void)\n{\n  if (g.k != V_ADDR)\n    return 0;\n  look(0);\n  return g.u.a;\n}\n",
        6,
        _msg("a", "V_ADDR"),
    ),
    "two_values": (
        "int f(struct value *v)\n{\n  if (v->k == V_D)\n    return 0;\n  return v->u.de;\n}\n",
        5,
        _msg("de", "V_D or V_E"),
    ),
    "address": (
        "char * view * view f(inout struct value *v)\n{\n  return &v->u.arr[0];\n}\n",
        3,
        _msg("arr", "V_E"),
    ),
    "unknown_store": (
        "int f(inout struct value *v, int k)\n{\n  v->k = V_STR;\n  v->k = k;\n  return use(v->u.s);\n}\n",
        5,
        _msg("s", "V_STR"),
    ),
    "base_reassigned": (
        "int f(struct value *v, struct value *w)\n{\n  if (v->k != V_STR)\n    return 0;\n"
        "  v = w;\n  return use(v->u.s);\n}\n",
        6,
        _msg("s", "V_STR"),
    ),
    "copy_stale_after_call": (
        "int f(inout struct value *v)\n{\n  enum vkind k = v->k;\n  poke(v);\n  if (k == V_STR)\n    return use(v->u.s);\n"
        "  return 0;\n}\n",
        6,
        _msg("s", "V_STR"),
    ),
    "copy_stale_after_store": (
        "int f(inout struct value *v)\n{\n  enum vkind k = v->k;\n  v->k = V_INT;\n  if (k == V_STR)\n    return use(v->u.s);\n"
        "  return 0;\n}\n",
        6,
        _msg("s", "V_STR"),
    ),
    "copy_changed_on_a_path": (
        "int f(struct value *v, int z)\n{\n  enum vkind k = v->k;\n  if (z)\n    k = V_STR;\n  if (k == V_STR)\n"
        "    return use(v->u.s);\n  return 0;\n}\n",
        7,
        _msg("s", "V_STR"),
    ),
    "global_pointer_call_forgets": (
        "struct value * view gp;\nlong f(void)\n{\n  if (gp->k != V_INT)\n    return 0;\n  look(0);\n  return gp->u.i;\n}\n",
        7,
        _msg("i", "V_INT"),
    ),
    "global_pointer_reassigned": (
        "struct value * view gp, * view other;\nlong f(void)\n{\n  if (gp->k != V_INT)\n    return 0;\n  gp = other;\n"
        "  return gp->u.i;\n}\n",
        7,
        _msg("i", "V_INT"),
    ),
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(BAD))
def test_tag_not_known(tmp_path, name, opt):
    body, line, msg = BAD[name]
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode != 0
    assert (line, "error", msg) in _diags(proc), proc.stderr


DECL = {
    "outside_struct": ("union(k) { int a when(V_INT); } x;", "a tagged union is a member of the struct that holds its tag 'k'"),
    "no_tag": ("struct s { union(k) { int a when(V_INT); } u; };", "the tag 'k' of a tagged union is not a member of its struct"),
    "tag_after": (
        "struct s { union(k) { int a when(V_INT); } u; int k; };",
        "the tag 'k' of a tagged union is declared before the union",
    ),
    "bitfield_tag": (
        "struct s { unsigned k:3; union(k) { int a when(V_INT); } u; };",
        "the tag 'k' of a tagged union is an integer or enum member, not a bit-field",
    ),
    "no_when": (
        "struct s { int k; union(k) { int a; } u; };",
        "a member of a tagged union says when its tag selects it: 'a' when(...)",
    ),
    "anonymous": (
        "struct s { int k; union(k) { int a when(V_INT); }; };",
        "a tagged union is a named member of the struct that holds its tag: union(k) { ... } name;",
    ),
    "array": ("struct s { int k; union(k) { int a when(V_INT); } u[2]; };", "a tagged union is not an array: its struct has one tag"),
}


@pytest.mark.parametrize("name", sorted(DECL))
def test_declaration_errors(tmp_path, name):
    decl, msg = DECL[name]
    proc = _compile(tmp_path, decl + "\n")
    assert proc.returncode != 0
    assert (1, "error", msg) in _diags(proc), proc.stderr


def test_regular_c_has_no_tagged_union(tmp_path):
    src = tmp_path / "c.c"
    src.write_text("struct s { int k; union(k) { int a; } u; };\n")
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-Wno-sign-conversion", "-Wno-unused-parameter", "-ccompat", "-c", str(src), "-o", str(tmp_path / "c.o")], capture_output=True, text=True)
    assert proc.returncode != 0 and "struct/union/enum name expected" in proc.stderr


# ---- a switch on the tag handles every variant, or has a default ----

def test_switch_misses_a_variant(tmp_path):
    body = (
        "int f(struct value *v)\n{\n  switch (v->k) {\n  case V_INT: return cast<int(int)>(v->u.i);\n"
        "  case V_STR: return use(v->u.s);\n  }\n  return 0;\n}\n"
    )
    proc = _compile(tmp_path, body, "-Wno-switch")
    msg = "a switch on the tag 'k' does not handle V_ADDR, V_D, V_E: add their cases, or a default"
    assert (3, "error", msg) in _diags(proc), proc.stderr


@pytest.mark.parametrize("copy", ["  enum vkind k = v->k;\n", "  enum vkind k;\n  k = v->k;\n"], ids=["init", "assign"])
def test_switch_on_a_copy_misses_a_variant(tmp_path, copy):
    body = (
        "int f(struct value *v)\n{\n" + copy + "  switch (k) {\n  case V_INT: return cast<int(int)>(v->u.i);\n"
        "  case V_STR: return use(v->u.s);\n  case V_ADDR: return cast<int(int)>(v->u.a);\n  }\n  return 0;\n}\n"
    )
    proc = _compile(tmp_path, body, "-Wno-switch")
    line = 3 + copy.count("\n")
    msg = "a switch on the tag 'k' does not handle V_D, V_E: add their cases, or a default"
    assert (line, "error", msg) in _diags(proc), proc.stderr


def test_tag_read_in_another_function_is_no_switch_value(tmp_path):
    """vregs start again per function: `*p` in g is where `v->k` was in f."""
    body = (
        "int fa(struct value *v)\n{\n  return v->k;\n}\n"
        "int ga(int *p)\n{\n  switch (*p) {\n  case 1: return 1;\n  }\n  return 0;\n}\n"
        "int ha(struct value *v)\n{\n  int k = v->k;\n  return k;\n}\n"
        "int ja(int *p)\n{\n  int k = *p;\n  switch (k) {\n  case 1: return 1;\n  }\n  return 0;\n}\n"
    )
    proc = _compile(tmp_path, body, "-Wno-switch")
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


@pytest.mark.parametrize(
    "store", ["  k = rd();\n", "  k += 1;\n", "  k++;\n", "  k = *q;\n"], ids=["call", "compound", "inc", "deref"]
)
def test_copy_of_the_tag_stored_again_is_not_checked(tmp_path, store):
    body = (
        "int rd(void);\nint f(struct value *v, int *q)\n{\n  int k = v->k;\n" + store +
        "  switch (k) {\n  case 1: return 1;\n  }\n  return 0;\n}\n"
    )
    proc = _compile(tmp_path, body, "-Wno-switch")
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_copy_of_the_tag_stored_again_from_the_tag_is_checked(tmp_path):
    body = (
        "int rd(void);\nint f(struct value *v)\n{\n  int k = rd();\n  k = v->k;\n"
        "  switch (k) {\n  case V_INT: return 1;\n  default: break;\n  }\n  k = rd();\n  k = v->k;\n"
        "  switch (k) {\n  case V_INT: return 1;\n  }\n  return 0;\n}\n"
    )
    proc = _compile(tmp_path, body, "-Wno-switch")
    msg = "a switch on the tag 'k' does not handle V_STR, V_ADDR, V_D, V_E: add their cases, or a default"
    assert _diags(proc) == [(12, "error", msg)], proc.stderr


def test_switch_on_a_plain_int_is_not_checked(tmp_path):
    body = "int f(int x)\n{\n  int k = x;\n  switch (k) {\n  case 1: return 1;\n  }\n  return 0;\n}\n"
    proc = _compile(tmp_path, body, "-Wno-switch")
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_switch_handles_every_variant(tmp_path):
    body = (
        "int f(struct value *v)\n{\n  switch (v->k) {\n  case V_INT: return cast<int(int)>(v->u.i);\n"
        "  case V_STR: return use(v->u.s);\n  case V_ADDR: return cast<int(int)>(v->u.a);\n  case V_D: case V_E: return v->u.de;\n"
        "  }\n  return 0;\n}\n"
    )
    proc = _compile(tmp_path, body, "-Wno-switch")
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


# ---- owned members: freed or moved before the variant changes ----

OWNED = """\
void * owned maybe malloc(unsigned n);
void free(drop void * owned maybe p);
void take(char * owned maybe p);
struct box { enum vkind k; union(k) { long i when(V_INT); char * owned maybe s when(V_STR); } u; };
"""

OWNED_GOOD = {
    "free_then_switch": (
        "void f(inout struct box *b)\n{\n  if (b->k == V_STR)\n    free(b->u.s);\n  b->k = V_INT;\n  b->u.i = 3;\n}\n"
    ),
    "move_then_switch": (
        "void f(inout struct box *b)\n{\n  if (b->k != V_STR)\n    return;\n  take(move(b->u.s));\n  b->k = V_INT;\n}\n"
    ),
    "set_after_free": (
        "void f(inout struct box *b)\n{\n  if (b->k == V_STR)\n    free(b->u.s);\n  b->k = V_STR;\n"
        "  b->u.s = malloc(4);\n}\n"
    ),
    "free_then_null": (
        "void f(inout struct box *b)\n{\n  if (b->k == V_STR) {\n    free(b->u.s);\n    b->u.s = nullptr;\n  }\n}\n"
    ),
    "free_then_new": (
        "void f(inout struct box *b)\n{\n  if (b->k != V_STR)\n    return;\n  free(b->u.s);\n  b->u.s = malloc(3);\n}\n"
    ),
    "local_freed": (
        "void f(void)\n{\n  struct box b = undefined;\n  b.k = V_STR;\n  b.u.s = malloc(4);\n  free(b.u.s);\n  b.k = V_INT;\n"
        "  b.u.i = 1;\n}\n"
    ),
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(OWNED_GOOD))
def test_owned_member_released(tmp_path, name, opt):
    proc = _compile(tmp_path, OWNED + OWNED_GOOD[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


OWNED_BAD = {
    "variant_change": (
        "void f(inout struct box *b)\n{\n  b->k = V_INT;\n  b->u.i = 3;\n}\n",
        3,
        "'u.s' may still own memory when 'k' changes: free or move it first",
    ),
    "tag_only": (
        "void f(inout struct box *b)\n{\n  b->k = V_INT;\n}\n",
        3,
        "'u.s' may still own memory when 'k' changes: free or move it first",
    ),
    "tag_only_in_branch": (
        "void f(inout struct box *b)\n{\n  if (b->k == V_STR)\n    b->k = V_INT;\n}\n",
        4,
        "'u.s' may still own memory when 'k' changes: free or move it first",
    ),
    "freed_still_selected": (
        "void f(inout struct box *b)\n{\n  if (b->k == V_STR)\n    free(b->u.s);\n}\n",
        4,
        "'u.s' is freed or moved out, and 'k' still selects it when the function returns: change the variant or give "
        "it a new value",
    ),
    "moved_still_selected": (
        "void f(inout struct box *b)\n{\n  if (b->k != V_STR)\n    return;\n  take(move(b->u.s));\n}\n",
        5,
        "'u.s' is freed or moved out, and 'k' still selects it when the function returns: change the variant or give "
        "it a new value",
    ),
    "written_over": (
        "void f(inout struct box *b)\n{\n  if (b->k != V_STR)\n    return;\n  b->u.s = malloc(4);\n}\n",
        5,
        "'u.s' may still own memory: free or move it before it is written over",
    ),
    "local_leak": (
        "void f(void)\n{\n  struct box b = undefined;\n  b.k = V_STR;\n  b.u.s = malloc(4);\n}\n",
        6,
        "'b.u.s' leaks: it still owns memory at the end of its scope",
    ),
    "freed_twice": (
        "void f(inout struct box *b)\n{\n  if (b->k != V_STR)\n    return;\n  free(b->u.s);\n  free(b->u.s);\n}\n",
        6,
        "'b->s' is freed twice",
    ),
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(OWNED_BAD))
def test_owned_member_lost(tmp_path, name, opt):
    body, line, msg = OWNED_BAD[name]
    owned_lines = OWNED.count("\n")
    proc = _compile(tmp_path, OWNED + body, opt)
    assert proc.returncode != 0
    assert (line + owned_lines, "error", msg) in _diags(proc), proc.stderr


def test_owned_member_in_plain_union_still_refused(tmp_path):
    proc = _compile(tmp_path, "union bad { char * owned p; int x; };\n")
    msg = "a union holds an owned member 'p': which member is live is not known, so it is never freed"
    assert (1, "error", msg) in _diags(proc), proc.stderr


def test_nested_member_path_is_named_in_place(tmp_path):
    """`v->in.p` is the member in place, not element 1 of an array of `in`s."""
    body = (
        "void free(drop void * owned maybe p);\nstruct o { int a; struct { char * owned maybe p; } in; };\n"
        "void f(inout struct o *v)\n{\n  free(v->in.p);\n  free(v->in.p);\n}\n"
    )
    proc = _compile(tmp_path, body)
    assert (6, "error", "'v->p' is freed twice") in _diags(proc), proc.stderr



NESTED = """\
struct N {
  int k;
  union(k) {
    struct P { int j; union(j) { long a when(1); long b when(2); } w; } s when(1);
    long i when(2);
    long q when(3);
    long r when(4);
  } u;
};
"""


@pytest.mark.parametrize("prelude", [False, True], ids=["alone", "after_prelude"])
def test_tagged_union_inside_a_member_of_another(tmp_path, prelude):
    """The inner union's entry grows the table while the outer's body is
    parsed: the outer's later when(...)s still land in its own entry."""
    body = NESTED + (
        "int f(struct N *n)\n{\n  switch (n->k) {\n  case 1: return n->u.s.j;\n  case 2: return cast<int(int)>(n->u.i);\n"
        "  case 3: return cast<int(int)>(n->u.q);\n  }\n  return 0;\n}\n"
        "long h(struct P *p)\n{\n  return p->w.b;\n}\n"
    )
    if prelude:
        proc = _compile(tmp_path, body, "-Wno-switch")
        base = 0
    else:
        src = tmp_path / "n.c"
        src.write_text(body)
        proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-Wno-sign-conversion", "-Wno-unused-parameter", "-c", "-fmax-errors=0", "-Wno-switch", str(src), "-o", str(tmp_path / "n.o")],
                              capture_output=True, text=True)
        base = PRELUDE_LINES
    d = _diags(proc)
    assert (12 - base, "error", "a switch on the tag 'k' does not handle 4: add its case, or a default") in d, proc.stderr
    assert (21 - base, "error", "'w.b' is used where 'j' may not be 2: test it first") in d, proc.stderr
    assert proc.returncode == 1, proc.stderr
