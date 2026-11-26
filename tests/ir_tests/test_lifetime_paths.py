"""-Wlifetime: path conditions and the values it follows (lifetime.c).

- Correlated conditions: a local or parameter (address not taken) tested
  against a constant more than once, or tested and assigned a constant, is a
  predicate; each block keeps up to 8 contexts, each knowing some predicates'
  values with its own state, and a test whose value a context knows sends it
  down one edge only.  `if (c) free(p); ... if (!c) free(p);` is clean, and
  so is a `done` flag.  `int ok = p != NULL; if (ok) *p` -- a local holding a
  pointer's null test tells about it while neither changed.
- A value with no name dereferenced where it may be null: `find(k)->v`,
  `(c ? p : q)->v`, `find(k)->next`, `keys()[i]` -- also when the call is
  inlined (-O1+): its result is as maybe as the call's type says.
- Maybe members of an array element at a constant index: `a[1].next` of a
  local or global array, `p[1].next` through a pointer.
- A variable whose address goes only to parameters that read (`T * owned
  const * view`) stays followed; the call reads it.
- An owned out-parameter's status kept in a local assigned more than once:
  the calls whose result it holds on each path decide.
- A borrow from several owners (`v = c ? p : q`, p on one path and q on
  another; 4 owners at most, past that not followed): used after any of them
  was freed or moved it "may be used after 'p', which it may borrow from, was
  freed"; per path, so a borrow that holds another owner there is clean.
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
void * owned malloc(unsigned n);
void free(drop void * owned maybe p);
int use(char *p);
struct node { int v; struct node * view maybe next; };
struct node * borrows(static) maybe find(int k);
struct node * borrows(static) must(int k);
static struct node gn;
static inline struct node * borrows(static) maybe ifind(int k)
{
  if (k)
    return nullptr;
  return &gn;
}
int peek(char * owned const *pp);
static inline int ipeek(char * owned const *pp)
{
  return **pp;
}
int pma(out void * owned maybe *memptr, unsigned long sz);
void fill(inout struct node *a);
struct node g[4];
void take(char * owned p);
char * borrows(s) bfirst(char *s);
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _compile(tmp_path, body, *flags, obj="t.o", compat=True):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    pre = ["-ccompat", "-Wlifetime"] if compat else []
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-write-strings", *pre, "-c", *flags, str(src), "-o", str(tmp_path / obj)],
        capture_output=True,
        text=True,
    )


def _diags(proc):
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


CLEAN = {
    # correlated conditions
    "both_ways": "void f(int c)\n{\n  char * owned p = malloc(1);\n  if (c)\n    free(p);\n  use(\"x\");\n"
    "  if (!c)\n    free(p);\n}\n",
    "same_way": "void f(int c)\n{\n  char * owned maybe p = 0;\n  if (c)\n    p = malloc(1);\n  use(\"x\");\n"
    "  if (c)\n    free(p);\n}\n",
    "done_flag": "void f(int c)\n{\n  char * owned p = malloc(1);\n  int done = 0;\n  if (c)\n  {\n    free(p);\n"
    "    done = 1;\n  }\n  if (!done)\n    free(p);\n}\n",
    "against_constant": "void f(int c)\n{\n  char * owned p = malloc(1);\n  if (c == 2)\n    free(p);\n"
    "  use(\"y\");\n  if (c != 2)\n    free(p);\n}\n",
    "loop_between": "void f(int c, int k)\n{\n  char * owned p = malloc(1);\n  for (int j = 0; j < k; j++)\n"
    "    use(\"z\");\n  if (c)\n    free(p);\n  else\n    use(p);\n  if (!c)\n    free(p);\n}\n",
    "bool_flag": "void f(int c)\n{\n  char * owned p = malloc(1);\n  _Bool kept = 1;\n  if (c)\n  {\n    free(p);\n"
    "    kept = 0;\n  }\n  if (kept)\n    free(p);\n}\n",
    # a local holding a null test
    "ok_local": "int f(struct node * maybe p)\n{\n  int ok = p != 0;\n  if (ok)\n    return p->v;\n  return 0;\n}\n",
    "not_local": "int f(struct node * maybe p)\n{\n  int z = !p;\n  if (z)\n    return 0;\n  return p->v;\n}\n",
    "ok_owned": "void f(void)\n{\n  char * owned maybe p = malloc(1);\n  _Bool ok = p;\n  if (ok)\n    free(p);\n}\n",
    # values with no name
    "must_result": "int f(int k)\n{\n  return must(k)->v;\n}\n",
    "tested_ternary": "int f(int c, struct node *p, struct node * maybe q)\n{\n  if (!q)\n    return 0;\n"
    "  return (c ? p : q)->v;\n}\n",
    "result_in_variable": "int f(int k)\n{\n  struct node * view maybe n = find(k);\n  if (!n)\n    return 0;\n"
    "  return n->v;\n}\n",
    # array elements
    "element_tested": "int f(void)\n{\n  struct node a[4] = undefined;\n  fill(a);\n  if (a[1].next)\n    return a[1].next->v;\n"
    "  return 0;\n}\n",
    "element_zero": "int f(void)\n{\n  struct node a[4] = undefined;\n  fill(a);\n  if (!a[0].next)\n    return 0;\n"
    "  return a[0].next->v;\n}\n",
    "pointer_element": "int f(struct node *p)\n{\n  if (p[1].next)\n    return p[1].next->v;\n  return 0;\n}\n",
    "global_element": "int f(void)\n{\n  if (g[2].next)\n    return g[2].next->v;\n  return 0;\n}\n",
    "call_result_copied": "int f(int k)\n{\n  struct node * view n = must(k);\n  if (n->next)\n"
    "    return n->next->v;\n  return 0;\n}\n",
    # f()->next is a new address each time: not followed, not reported
    "call_result_member": "int f(int k)\n{\n  if (must(k)->next)\n    return must(k)->next->v;\n  return 0;\n}\n",
    # read-only address arguments
    "readonly_freed": "void f(void)\n{\n  char * owned p = malloc(1);\n  peek(&p);\n  free(p);\n}\n",
    "readonly_inlined": "void f(void)\n{\n  char * owned p = malloc(1);\n  ipeek(&p);\n  free(p);\n}\n",
    # status locals assigned more than once
    "status_reused": "void f(void)\n{\n  void * owned maybe m;\n  int r = 0;\n  r = pma(&m, 16);\n  if (r)\n"
    "    return;\n  free(m);\n}\n",
    "status_two_calls": "void f(void)\n{\n  void * owned maybe m;\n  void * owned maybe n;\n  int r = pma(&m, 16);\n"
    "  if (r)\n    return;\n  r = pma(&n, 8);\n  if (r)\n  {\n    free(m);\n    return;\n  }\n  free(m);\n  free(n);\n}\n",
    # r stays 0 where pma is not called: m is not set there, nothing leaks
    "status_not_always_set": "void f(int k)\n{\n  void * owned maybe m = nullptr;\n  int r = 0;\n  if (k)\n"
    "    r = pma(&m, 16);\n  if (r)\n    return;\n  if (k)\n    free(m);\n}\n",
    "status_either_call": "void f(int k)\n{\n  void * owned maybe m;\n  int r;\n  if (k)\n    r = pma(&m, 16);\n"
    "  else\n    r = pma(&m, 32);\n  if (r)\n    return;\n  free(m);\n}\n",
    # borrows from several owners
    "owners_used_before": "int f(int c)\n{\n  char * owned p = malloc(1);\n  char * owned q = malloc(1);\n"
    "  char * view v = c ? p : q;\n  int r = use(v);\n  free(p);\n  free(q);\n  return r;\n}\n",
    "owners_borrowed_again": "int f(int c)\n{\n  char * owned p = malloc(1);\n  char * owned q = malloc(1);\n"
    "  char * view v = c ? p : q;\n  free(p);\n  v = q;\n  int r = use(v);\n  free(q);\n  return r;\n}\n",
    "owners_other_path": "int f(int c)\n{\n  char * owned p = malloc(1);\n  char * owned q = malloc(1);\n"
    "  char * view v = p;\n  int r = use(v);\n  v = q;\n  free(p);\n  r += use(v);\n  free(q);\n  return r;\n}\n",
    "owners_correlated": "int f(int c)\n{\n  char * owned p = malloc(1);\n  char * owned q = malloc(1);\n"
    "  char * view v;\n  if (c)\n    v = p;\n  else\n    v = q;\n  if (!c)\n  {\n    free(p);\n    p = malloc(1);\n"
    "  }\n  int r = use(v);\n  free(p);\n  free(q);\n  return r;\n}\n",
    "owners_in_loop": "int f(int n)\n{\n  char * owned p = malloc(1);\n  char * owned q = malloc(1);\n  int r = 0;\n"
    "  for (int i = 0; i < n; i++)\n  {\n    char * view v = i & 1 ? p : q;\n    r += use(v);\n  }\n  free(p);\n"
    "  free(q);\n  return r;\n}\n",
    "owner_or_view_used_before": "int f(int c, inout char *s)\n{\n  char * owned p = malloc(1);\n"
    "  char * view v = c ? p : s;\n  int r = use(v);\n  free(p);\n  return r;\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_clean(tmp_path, name, opt):
    assert _check(tmp_path, CLEAN[name], opt) == []


BAD = {
    # five owners: followed (8 at most); past 8 it is said, not followed
    "owners_five": (
        "int f(int c)\n{\n  char * owned a = malloc(1);\n  char * owned b = malloc(1);\n"
    "  char * owned d = malloc(1);\n  char * owned e = malloc(1);\n  char * owned h = malloc(1);\n  char * view v = a;\n"
    "  if (c == 1)\n    v = b;\n  if (c == 2)\n    v = d;\n  if (c == 3)\n    v = e;\n  if (c == 4)\n    v = h;\n"
    "  free(a);\n  int r = use(v);\n  free(b);\n  free(d);\n  free(e);\n  free(h);\n  return r;\n}\n",
        [(18, "warning", "'v' may be used after 'a', which it may borrow from, was freed"), (17, "note", "'a' was freed here")],
    ),
    "owners_nine": (
        "int f(int c)\n{\n" + "".join(f"  char * owned o{k} = malloc(1);\n" for k in range(9)) + "  char * view v = o0;\n"
        + "".join(f"  if (c == {k})\n    v = o{k};\n" for k in range(1, 9))
        + "  int r = use(v);\n" + "".join(f"  free(o{k});\n" for k in range(9)) + "  return r;\n}\n",
        [(12, "warning", "'v' may borrow from more than 8 owners: the lifetime check does not follow it")],
    ),
    # a copy of it is not followed either, and that is said too
    "owners_nine_copied": (
        "int f(int c)\n{\n" + "".join(f"  char * owned o{k} = malloc(1);\n" for k in range(9))
        + "  char * view v = o0;\n  char * view u = o0;\n"
        + "".join(f"  if (c == {k})\n    v = o{k};\n" for k in range(1, 9))
        + "  char * view w = v;\n  free(o8);\n  int r = use(w) + use(u);\n"
        + "".join(f"  free(o{k});\n" for k in range(8)) + "  return r;\n}\n",
        [(12, "warning", "'v' may borrow from more than 8 owners: the lifetime check does not follow it"),
         (30, "warning", "'w' may borrow from more than 8 owners: the lifetime check does not follow it")],
    ),
    "freed_twice_same_way": (
        "void f(int c)\n{\n  char * owned p = malloc(1);\n  if (c)\n    free(p);\n  if (c)\n    free(p);\n}\n",
        [(7, "warning", "'p' is freed twice"), (5, "note", "'p' was freed here"),
         (8, "warning", "'p' leaks: it still owns memory at the end of its scope")],
    ),
    "assigned_between": (
        "void f(int c)\n{\n  char * owned p = malloc(1);\n  if (c)\n    free(p);\n  c = 5;\n  if (!c)\n    free(p);\n}\n",
        [(9, "warning", "'p' leaks: it may still own memory at the end of its scope")],
    ),
    "ok_reassigned": (
        "int f(struct node * maybe p)\n{\n  int ok = p != 0;\n  ok = 1;\n  if (ok)\n    return p->v;\n"
        "  return 0;\n}\n",
        [(6, "warning", "'p' may be null where it is dereferenced")],
    ),
    "ok_pointer_reassigned": (
        "int f(struct node * maybe p, struct node * maybe q)\n{\n  int ok = p != 0;\n  p = q;\n"
        "  if (ok)\n    return p->v;\n  return 0;\n}\n",
        [(6, "warning", "'p' may be null where it is dereferenced")],
    ),
    "call_result": (
        "int f(int k)\n{\n  return find(k)->v;\n}\n",
        [(3, "warning", "a pointer that may be null is dereferenced")],
    ),
    "call_result_member": (
        "int f(int k)\n{\n  return find(k)->next != 0;\n}\n",
        [(3, "warning", "a pointer that may be null is dereferenced")],
    ),
    "inlined_result": (
        "int f(int k)\n{\n  return ifind(k)->v;\n}\n",
        [(3, "warning", "a pointer that may be null is dereferenced")],
    ),
    "inlined_result_stored": (
        "int f(int k)\n{\n  struct node * borrows(static) q = ifind(k);\n  return q->v;\n}\n",
        [(3, "warning", "a pointer that may be null is stored in 'q', which is not maybe")],
    ),
    "ternary": (
        "int f(int c, struct node *p, struct node * maybe q)\n{\n  return (c ? p : q)->v;\n}\n",
        [(3, "warning", "a pointer that may be null is dereferenced")],
    ),
    "two_calls_one_line": (
        "int f(int k)\n{\n  return find(k)->v + find(k + 1)->v;\n}\n",
        [(3, "warning", "a pointer that may be null is dereferenced")],
    ),
    "element_untested": (
        "int f(void)\n{\n  struct node a[4] = undefined;\n  fill(a);\n  return a[1].next->v;\n}\n",
        [(5, "warning", "'a[1].next' may be null where it is dereferenced")],
    ),
    "element_other_index": (
        "int f(void)\n{\n  struct node a[4] = undefined;\n  fill(a);\n  if (!a[0].next)\n    return 0;\n"
        "  return a[2].next->v;\n}\n",
        [(7, "warning", "'a[2].next' may be null where it is dereferenced")],
    ),
    "element_forgotten": (
        "int f(void)\n{\n  struct node a[4] = undefined;\n  fill(a);\n  if (!a[1].next)\n    return 0;\n  fill(a);\n"
        "  return a[1].next->v;\n}\n",
        [(8, "warning", "'a[1].next' may be null where it is dereferenced")],
    ),
    "element_stored_null": (
        "int f(void)\n{\n  struct node a[4] = undefined;\n  fill(a);\n  a[1].next = 0;\n  return a[1].next->v;\n}\n",
        [(6, "warning", "'a[1].next' is null where it is dereferenced")],
    ),
    "pointer_element": (
        "int f(struct node *p)\n{\n  if (p[1].next)\n    return p[1].next->v;\n  return p[2].next->v;\n}\n",
        [(5, "warning", "'p[2].next' may be null where it is dereferenced")],
    ),
    "global_element": (
        "int f(void)\n{\n  if (g[1].next)\n    return g[1].next->v;\n  return g[2].next->v;\n}\n",
        [(5, "warning", "'g[2].next' may be null where it is dereferenced")],
    ),
    "readonly_leak": (
        "void f(void)\n{\n  char * owned p = malloc(1);\n  peek(&p);\n}\n",
        [(5, "warning", "'p' leaks: it still owns memory at the end of its scope")],
    ),
    "readonly_after_free": (
        "void f(void)\n{\n  char * owned p = malloc(1);\n  free(p);\n  peek(&p);\n}\n",
        [(5, "warning", "'p' is used after it was freed"), (4, "note", "'p' was freed here")],
    ),
    "readonly_inlined_leak": (
        "void f(void)\n{\n  char * owned p = malloc(1);\n  ipeek(&p);\n}\n",
        [(5, "warning", "'p' leaks: it still owns memory at the end of its scope")],
    ),
    # borrows from several owners
    "owners_ternary": (
        "int f(int c)\n{\n  char * owned p = malloc(1);\n  char * owned q = malloc(1);\n"
        "  char * view v = c ? p : q;\n  free(p);\n  int r = use(v);\n  free(q);\n  return r;\n}\n",
        [(7, "warning", "'v' may be used after 'p', which it may borrow from, was freed"), (6, "note", "'p' was freed here")],
    ),
    "owners_two_paths": (
        "int f(int c)\n{\n  char * owned p = malloc(1);\n  char * owned q = malloc(1);\n  char * view v;\n"
        "  if (c)\n    v = p;\n  else\n    v = q;\n  free(q);\n  int r = use(v);\n  free(p);\n  return r;\n}\n",
        [(11, "warning", "'v' may be used after 'q', which it may borrow from, was freed"), (10, "note", "'q' was freed here")],
    ),
    "owners_moved": (
        "int f(int c)\n{\n  char * owned p = malloc(1);\n  char * owned q = malloc(1);\n"
        "  char * view v = c ? p + 1 : q;\n  take(move(q));\n  int r = use(v);\n  free(p);\n  return r;\n}\n",
        [(7, "warning", "'v' may be used after 'q', which it may borrow from, was moved"), (6, "note", "'q' was moved here")],
    ),
    "owners_through_calls": (
        "int f(int c)\n{\n  char * owned p = malloc(1);\n  char * owned q = malloc(1);\n"
        "  char * view v = c ? bfirst(p) : bfirst(q);\n  free(q);\n  int r = use(v);\n  free(p);\n  return r;\n}\n",
        [(7, "warning", "'v' may be used after 'q', which it may borrow from, was freed"), (6, "note", "'q' was freed here")],
    ),
    "owner_or_view": (
        "int f(int c, inout char *s)\n{\n  char * owned p = malloc(1);\n  char * view v = c ? p : s;\n  free(p);\n"
        "  return use(v);\n}\n",
        [(6, "warning", "'v' may be used after 'p', which it may borrow from, was freed"), (5, "note", "'p' was freed here")],
    ),
}


@pytest.mark.parametrize("name", sorted(BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_reported(tmp_path, name, opt):
    body, want = BAD[name]
    assert sorted(_check(tmp_path, body, opt)) == sorted(want)


def test_inline_twice_once(tmp_path):
    """A static inline called twice reports its body once, at every -O."""
    body = (
        "static inline void two(int c)\n{\n  char * owned p = malloc(1);\n  if (c)\n    free(p);\n"
        "  if (c)\n    free(p);\n}\nvoid a(int c)\n{\n  two(c);\n  two(!c);\n}\n"
    )
    want = _check(tmp_path, body, "-O0")
    assert [d[2] for d in want].count("'p' is freed twice") == 1
    for opt in LEVELS[1:]:
        assert _check(tmp_path, body, opt) == want


def test_owners_inline_twice_once(tmp_path):
    """Several owners in a static inline called twice: reported once, at every -O."""
    body = (
        "static inline int two(int c)\n{\n  char * owned p = malloc(1);\n  char * owned q = malloc(1);\n"
        "  char * view v = c ? p : q;\n  free(p);\n  int r = use(v);\n  free(q);\n  return r;\n}\n"
        "int a(int c)\n{\n  return two(c) + two(!c);\n}\n"
    )
    want = _check(tmp_path, body, "-O0")
    assert want == BAD["owners_ternary"][1]
    for opt in LEVELS[1:]:
        assert _check(tmp_path, body, opt) == want


ONE = BAD["freed_twice_same_way"][0]


def test_wno_lifetime(tmp_path):
    proc = _compile(tmp_path, ONE + BAD["call_result"][0].replace("int f(", "int h("), "-Wno-lifetime")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_werror_lifetime(tmp_path):
    proc = _compile(tmp_path, BAD["call_result"][0], "-Werror=lifetime")
    assert proc.returncode != 0
    assert [d[:2] for d in _diags(proc)] == [(3, "error")]


def test_wno_error_lifetime(tmp_path):
    proc = _compile(tmp_path, BAD["call_result"][0], "-Werror", "-Wno-error=lifetime")
    assert proc.returncode == 0, proc.stderr
    assert [d[:2] for d in _diags(proc)] == [(3, "warning")]


def test_ccompat_without_wlifetime(tmp_path):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + BAD["call_result"][0])
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-Wno-write-strings", "-ccompat", "-c", str(src), "-o", str(tmp_path / "t.o")], capture_output=True, text=True)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_safe_default(tmp_path):
    """Without -ccompat the dialect and -Wlifetime are on (an error)."""
    proc = _compile(tmp_path, BAD["call_result"][0] + CLEAN["both_ways"].replace("void f(", "void h("), compat=False)
    assert proc.returncode != 0, proc.stderr
    assert [d[1:] for d in _diags(proc)] == [("error", "a pointer that may be null is dereferenced")]


def test_pragma_ignored(tmp_path):
    body = (
        '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wlifetime"\n'
        + BAD["call_result"][0]
        + "#pragma GCC diagnostic pop\n"
        + BAD["call_result"][0].replace("int f(", "int h(")
    )
    assert _check(tmp_path, body) == [(10, "warning", "a pointer that may be null is dereferenced")]


@pytest.mark.parametrize("opt", LEVELS)
def test_code_unchanged(tmp_path, opt):
    corpus = "".join(
        body.replace(" f(", f" f{i}(", 1) for i, body in enumerate([b for b, _ in BAD.values()] + list(CLEAN.values()))
    )
    plain = _compile(tmp_path, corpus, opt, "-Wno-lifetime", obj="plain.o")
    checked = _compile(tmp_path, corpus, opt, obj="checked.o")
    assert plain.returncode == 0 and checked.returncode == 0, checked.stderr
    assert (tmp_path / "plain.o").read_bytes() == (tmp_path / "checked.o").read_bytes()
