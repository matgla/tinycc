"""yasc `static T *?borrows(owner) g;`: a file-scope global that points into
what another global owns (declarator.c yasc_gborrows, lifetime.c gb_check).

The rule holds at every function's boundary: on entry each such global
points into its owners' current memory, or is null.  Where an owner is
freed, moved out or assigned, the globals borrowing from it are dead until
they are stored again: read while dead, left dead where the function
returns, or dead at a call that may read them, they are reported.  A store
into one takes a pointer into its owners' memory, null, or static storage.
Every case runs at each -O: the check reads the raw IR.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
void *?owned malloc(unsigned long n);
void *?@(owned, takes(p)) realloc(void *?owned p, unsigned long n);
void free(drop void *?owned p);
void other(void);
char *?borrows(s) strchr(char *s, int c);
static unsigned char *?owned(free) zfile;
static unsigned char *?owned(free) zother;
static const unsigned char *?borrows(zfile) ztrans;
static const unsigned char *?borrows(zfile, zother) zboth;
static const char lit[] = "abc";
"""
PRELUDE_LINES = PRELUDE.count("\n")
LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _compile(tmp_path, body, *flags, prelude=PRELUDE):
    src = tmp_path / "t.c"
    src.write_text(prelude + body)
    return subprocess.run(
        [str(TCC), "-Wlifetime", "-Wno-missing-prototypes", "-fmax-errors=50", "-c", *flags, str(src), "-o",
         str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _diags(tmp_path, body, *flags):
    """(line in BODY, message) for every lifetime diagnostic."""
    proc = _compile(tmp_path, body, *flags)
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        where = parts[0].rsplit(":", 1)
        if len(parts) == 3 and parts[1] in ("warning", "error") and len(where) == 2 and where[1].isdigit():
            at = int(where[1]) - PRELUDE_LINES
            if at > 0 and "defined but not used" not in parts[2]:
                out.append((at, parts[2]))
    return out


# a line in a message is the file's: BODY's line + the prelude's
def _dead_read(b, o, line):
    return f"'{b}' may be used after '{o}', which it borrows from, was freed or changed (line {line + PRELUDE_LINES})"


def _dead_exit(b, o, fn):
    return f"'{b}' borrows from '{o}', which is freed or changed here: set '{b}' again (or to nullptr) before '{fn}' returns"


def _dead_call(b, o, line, callee):
    return (f"'{b}' borrows from '{o}', which was freed or changed at line {line + PRELUDE_LINES}: set '{b}' again "
            f"(or to nullptr) before calling '{callee}'")


def _bad_store(b, o):
    return f"'{b}' borrows from '{o}': it can hold only a pointer into what '{o}' owns, null or static storage"


CLEAN = {
    "load_through_a_local": """\
int load(unsigned long off)
{
  unsigned char *?owned(free) b = malloc(64);
  if (!b)
    return 0;
  free(move(zfile));
  zfile = move(b);
  const unsigned char *base = zfile;
  ztrans = base + off;
  zboth = zfile + 1;
  return 1;
}
""",
    # set to null first: the owner's free leaves it alone (detached)
    "detached_first": """\
void reset(void)
{
  ztrans = nullptr;
  zboth = nullptr;
  free(move(zfile));
  zfile = nullptr;
  other();
}
""",
    "detached_in_a_loop": """\
int loop(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    s += ztrans ? ztrans[0] : 0;
    ztrans = nullptr;
    zboth = nullptr;
    free(move(zfile));
    zfile = nullptr;
  }
  return s;
}
""",
    "reset_then_call": """\
void reset(void)
{
  free(move(zfile));
  zfile = nullptr;
  ztrans = nullptr;
  zboth = nullptr;
  other();
}
""",
    "realloc_both_paths": """\
int grow(unsigned long n)
{
  unsigned char *?owned(free) g = realloc(move(zfile), n);
  if (!g)
  {
    ztrans = nullptr;
    zboth = nullptr;
    return 0;
  }
  zfile = move(g);
  ztrans = zfile;
  zboth = zfile;
  return 1;
}
""",
    "through_a_borrowing_call": """\
void name(void)
{
  if (zfile)
    zboth = cast<ptr(const unsigned char *)>(strchr(cast<ptr(const char *)>(zfile), 'x'));
}
""",
    "static_storage_and_null": """\
void fixed(int k)
{
  ztrans = k ? cast<ptr(const unsigned char *)>(lit) : nullptr;
}
""",
    "a_copy_of_another_borrower": "void copy(void)\n{\n  zboth = ztrans;\n}\n",
    "reader": "int reader(void)\n{\n  return (ztrans ? ztrans[0] : 0) + (zboth ? zboth[0] : 0);\n}\n",
    "an_owner_alone_untouched": "int peek(void)\n{\n  return zother ? zother[0] : 0;\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_clean(tmp_path, name, opt):
    proc = _compile(tmp_path, CLEAN[name], opt)
    assert proc.returncode == 0, proc.stderr
    assert _diags(tmp_path, CLEAN[name], opt) == []


BAD = {
    "read_after_free": (
        "int uaf(void)\n{\n  free(move(zfile));\n  zfile = nullptr;\n  int r = ztrans ? ztrans[0] : 0;\n"
        "  ztrans = nullptr;\n  zboth = nullptr;\n  return r;\n}\n",
        [(5, _dead_read("ztrans", "zfile", 3))],
    ),
    "left_dead_at_return": (
        "void reset(void)\n{\n  free(move(zfile));\n  zfile = nullptr;\n  ztrans = nullptr;\n}\n",
        [(3, _dead_exit("zboth", "zfile", "reset"))],
    ),
    "dead_at_a_call": (
        "void reset(void)\n{\n  free(move(zfile));\n  zfile = nullptr;\n  other();\n  ztrans = nullptr;\n"
        "  zboth = nullptr;\n}\n",
        [(5, _dead_call("ztrans", "zfile", 3, "other")), (5, _dead_call("zboth", "zfile", 3, "other"))],
    ),
    "moved_into_a_local": (
        "void out(void)\n{\n  unsigned char *?owned(free) p = move(zfile);\n  zfile = nullptr;\n"
        "  ztrans = nullptr;\n  zboth = nullptr;\n  free(move(p));\n  int k = ztrans ? 1 : 0;\n  (void)k;\n}\n",
        [],
    ),
    "a_second_owner": (
        "void drop_other(void)\n{\n  free(move(zother));\n  zother = nullptr;\n}\n",
        [(3, _dead_exit("zboth", "zother", "drop_other"))],
    ),
    "assigned_owner": (
        "void swap(unsigned char *?owned(free) n)\n{\n  free(move(zfile));\n  zfile = move(n);\n"
        "  ztrans = nullptr;\n}\n",
        [(3, _dead_exit("zboth", "zfile", "swap"))],
    ),
    "next_iteration": (
        "int loop(int n)\n{\n  int s = 0;\n  for (int i = 0; i < n; i++)\n  {\n    s += ztrans ? ztrans[0] : 0;\n"
        "    free(move(zfile));\n    zfile = nullptr;\n  }\n"
        "  ztrans = nullptr;\n  zboth = nullptr;\n  return s;\n}\n",
        [(6, _dead_read("ztrans", "zfile", 7))],
    ),
    "another_owners_memory": (
        "void wrong(void)\n{\n  ztrans = zother;\n}\n",
        [(3, _bad_store("ztrans", "zfile"))],
    ),
    "unknown_memory": (
        "void wrong(const unsigned char *?*p)\n{\n  ztrans = *p;\n}\n",
        [(3, _bad_store("ztrans", "zfile"))],
    ),
}


@pytest.mark.parametrize("name", sorted(BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_reported(tmp_path, name, opt):
    body, want = BAD[name]
    got = _diags(tmp_path, body, opt)
    for d in want:
        assert d in got, got
    if not want:
        assert got == [], got


def test_moved_into_a_local_then_read(tmp_path):
    """Moved out into an owned local (not freed yet): the borrowers die with the move."""
    body = ("void out(void)\n{\n  unsigned char *?owned(free) p = move(zfile);\n  zfile = nullptr;\n"
            "  int k = ztrans ? 1 : 0;\n  (void)k;\n  ztrans = nullptr;\n  zboth = nullptr;\n  free(move(p));\n}\n")
    assert (5, _dead_read("ztrans", "zfile", 3)) in _diags(tmp_path, body, "-O0")


def test_returned_as_owned(tmp_path):
    body = "unsigned char *?owned take(void)\n{\n  unsigned char *?owned r = move(zfile);\n  zfile = nullptr;\n  return r;\n}\n"
    got = _diags(tmp_path, body, "-O0")
    assert (3, _dead_exit("ztrans", "zfile", "take")) in got, got


MOVED_OUT = {
    "into_an_owned_local": "unsigned char *?owned take(void)\n{\n  unsigned char *?owned r = move(zfile);\n  return r;\n}\n",
    "returned": "unsigned char *?owned take(void)\n{\n  return move(zfile);\n}\n",
}


@pytest.mark.parametrize("name", sorted(MOVED_OUT))
@pytest.mark.parametrize("opt", LEVELS)
def test_moved_out_without_a_store(tmp_path, name, opt):
    """Ownership leaves the owner with no store into it: the borrowers die all the same."""
    got = _diags(tmp_path, MOVED_OUT[name], opt)
    assert (3, _dead_exit("ztrans", "zfile", "take")) in got, got


def test_inlined_helper_once(tmp_path):
    """A static helper inlined twice that leaves a borrower dead: said once, in the helper."""
    body = ("static inline void drop_zone(void)\n{\n  free(move(zfile));\n  zfile = nullptr;\n}\n"
            "void a(void)\n{\n  drop_zone();\n  ztrans = nullptr;\n  zboth = nullptr;\n}\n"
            "void b(void)\n{\n  drop_zone();\n  ztrans = nullptr;\n  zboth = nullptr;\n}\n")
    for opt in LEVELS:
        got = _diags(tmp_path, body, opt)
        assert got == [(3, _dead_exit("ztrans", "zfile", "drop_zone")), (3, _dead_exit("zboth", "zfile", "drop_zone"))], (opt, got)


def test_ccompat_warns_and_wno_lifetime_silences(tmp_path):
    """In yasc -Wlifetime is an error that cannot be lowered; under -ccompat
    (a `#pragma yasc` file) it is a warning, and -Wno-lifetime silences it."""
    body = "void reset(void)\n{\n  free(move(zfile));\n  zfile = nullptr;\n}\n"
    warned = _compile(tmp_path, body, "-ccompat")
    assert warned.returncode == 0, warned.stderr
    assert "warning: " + _dead_exit("ztrans", "zfile", "reset") in warned.stderr
    quiet = _compile(tmp_path, body, "-ccompat", "-Wno-lifetime")
    assert quiet.returncode == 0 and "borrows from" not in quiet.stderr, quiet.stderr


ERRORS = {
    "unknown_name": ("static const char *?borrows(nope) x;", "borrows(nope): no global 'nope' is declared before it"),
    "owner_owns_nothing": (
        "static char *plain;\nstatic const char *?borrows(plain) x;",
        "borrows(plain): 'plain' owns nothing: a global borrows from a global whose pointer says owned",
    ),
    "a_function": (
        "void fn(void);\nstatic const char *?borrows(fn) x;",
        "borrows(fn): no global 'fn' is declared before it",
    ),
    "automatic_local": (
        "void f(void)\n{\n  const unsigned char *?borrows(zfile) x = nullptr;\n  (void)x;\n}",
        "borrows(zfile): only a function's result, a struct member, a global or a static names its sources",
    ),
    "typedef": (
        "typedef const unsigned char *?borrows(zfile) T;",
        "borrows(zfile): only a function's result, a struct member, a global or a static names its sources",
    ),
    "declared_again_without": (
        "static const unsigned char *?ztrans;",
        "'ztrans' is declared again with another borrows(...)",
    ),
    "declared_again_other_owner": (
        "static const unsigned char *?borrows(zother) ztrans;",
        "'ztrans' is declared again with another borrows(...)",
    ),
}


@pytest.mark.parametrize("name", sorted(ERRORS))
def test_errors(tmp_path, name):
    decl, msg = ERRORS[name]
    proc = _compile(tmp_path, decl + "\n", "-fsyntax-only")
    assert proc.returncode != 0
    assert [l.split("error: ", 1)[1] for l in proc.stderr.splitlines() if "error: " in l][0] == msg


def test_extern_then_definition(tmp_path):
    body = ("extern const unsigned char *?borrows(zfile) zext;\nconst unsigned char *?borrows(zfile) zext;\n"
            "void set(void)\n{\n  zext = zfile;\n}\n")
    proc = _compile(tmp_path, body, "-O0")
    assert proc.returncode == 0, proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_code_unchanged(tmp_path, opt):
    """The check only reads: the object is the same as with -Wno-lifetime."""
    body = CLEAN["load_through_a_local"] + CLEAN["reader"]
    a = tmp_path / "a.o"
    b = tmp_path / "b.o"
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    subprocess.run([str(TCC), opt, "-c", str(src), "-o", str(a)], check=True, capture_output=True)
    subprocess.run([str(TCC), opt, "-Wno-lifetime", "-c", str(src), "-o", str(b)], check=True, capture_output=True)
    assert a.read_bytes() == b.read_bytes()


# --- borrows(member) members of an object that outlives the function ---
#
# The same rule for `struct zone { T *?owned file; T *?borrows(file) trans; }`:
# a global struct's members (`g.trans`, nested, array elements at a constant
# index) are followed like borrowing globals, and an object reached through a
# view parameter keeps no dead member at a call or where the function returns
# -- even when the function never names the borrowing member.  A local
# object dies with the function: only reads of it are said.

MPRELUDE = """\
#pragma yasc
void *?owned malloc(unsigned long n);
void free(drop void *?owned p);
void other(void);
void peek(struct zone *z);
struct zone { unsigned char *?owned(free) file; const unsigned char *?borrows(file) trans; char buf[8];
              const char *?borrows(buf) cur; };
struct outer { int k; struct zone z; };
static struct zone g;
static struct outer go;
static struct zone arr[2];
static unsigned char *?owned(free) stray;
"""
MPRELUDE_LINES = MPRELUDE.count("\n")


def _mdiags(tmp_path, body, *flags):
    proc = _compile(tmp_path, body, *flags, prelude=MPRELUDE)
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        where = parts[0].rsplit(":", 1)
        if len(parts) == 3 and parts[1] in ("warning", "error") and len(where) == 2 and where[1].isdigit():
            at = int(where[1]) - MPRELUDE_LINES
            if at > 0 and "defined but not used" not in parts[2]:
                out.append((at, parts[2]))
    return out


def _m_read(b, o, line):
    return f"'{b}' may be used after '{o}', which it borrows from, was freed or changed (line {line + MPRELUDE_LINES})"


def _m_call(b, o, line, callee):
    return (f"'{b}' borrows from '{o}', which was freed or changed at line {line + MPRELUDE_LINES}: set '{b}' again "
            f"(or to nullptr) before calling '{callee}'")


M_CLEAN = {
    "global_reset": "void ok(void)\n{\n  free(move(g.file));\n  g.file = nullptr;\n  g.trans = nullptr;\n}\n",
    "global_own_array": "void at(void)\n{\n  g.cur = g.buf + 2;\n  if (g.file)\n    g.trans = g.file + 1;\n}\n",
    "param_reset": ("void ok(inout struct zone *z)\n{\n  free(move(z->file));\n  z->file = nullptr;\n"
                    "  z->trans = nullptr;\n}\n"),
    "param_reload": ("void reload(inout struct zone *z, unsigned char *?owned(free) n)\n{\n  free(move(z->file));\n"
                     "  z->file = move(n);\n  z->trans = z->file;\n}\n"),
    "local_object": ("int local(void)\n{\n  struct zone l = {nullptr, nullptr, {0}, nullptr};\n  l.file = malloc(4);\n"
                     "  if (!l.file)\n    return 0;\n  l.trans = l.file;\n  free(move(l.file));\n  l.file = nullptr;\n"
                     "  return 1;\n}\n"),
    "param_other_member": "void set(inout struct zone *z)\n{\n  z->trans = nullptr;\n  other();\n}\n",
}


@pytest.mark.parametrize("name", sorted(M_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_member_clean(tmp_path, name, opt):
    assert _mdiags(tmp_path, M_CLEAN[name], opt) == []


M_BAD = {
    "global_read_after_free": (
        "int bad(void)\n{\n  free(move(g.file));\n  g.file = nullptr;\n  int r = g.trans ? g.trans[0] : 0;\n"
        "  g.trans = nullptr;\n  return r;\n}\n",
        [(5, _m_read("g.trans", "g.file", 3))],
    ),
    "global_left_dead": (
        "void leaves(void)\n{\n  free(move(g.file));\n  g.file = nullptr;\n}\n",
        [(3, _dead_exit("g.trans", "g.file", "leaves"))],
    ),
    "nested_at_a_call": (
        "void nested(void)\n{\n  free(move(go.z.file));\n  go.z.file = nullptr;\n  other();\n  go.z.trans = nullptr;\n}\n",
        [(5, _m_call("go.z.trans", "go.z.file", 3, "other"))],
    ),
    "array_element": (
        "void elem(void)\n{\n  free(move(arr[1].file));\n  arr[1].file = nullptr;\n}\n",
        [(3, _dead_exit("arr[1].trans", "arr[1].file", "elem"))],
    ),
    "global_other_memory": (
        "void wrong(void)\n{\n  g.trans = stray;\n}\n",
        [(3, _bad_store("g.trans", "g.file"))],
    ),
    "param_never_names_it": (
        "void drop_file(inout struct zone *z)\n{\n  free(move(z->file));\n  z->file = nullptr;\n}\n",
        [(3, _dead_exit("z->trans", "z->file", "drop_file"))],
    ),
    "param_at_a_call": (
        "void between(inout struct zone *z)\n{\n  free(move(z->file));\n  z->file = nullptr;\n  other();\n"
        "  z->trans = nullptr;\n}\n",
        [(5, _m_call("z->trans", "z->file", 3, "other"))],
    ),
    "param_handed_on": (
        "void handed(inout struct zone *z)\n{\n  free(move(z->file));\n  z->file = nullptr;\n  peek(z);\n"
        "  z->trans = nullptr;\n}\n",
        [(5, _m_call("z->trans", "z->file", 3, "peek"))],
    ),
}


@pytest.mark.parametrize("name", sorted(M_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_member_reported(tmp_path, name, opt):
    body, want = M_BAD[name]
    got = _mdiags(tmp_path, body, opt)
    for d in want:
        assert d in got, got


# --- the limits lifted: indexes, an owner's address, detached items,
# --- objects through pointer locals and owned pointers ---

XPRELUDE = """\
#pragma yasc
void *?owned malloc(unsigned long n);
void free(drop void *?owned p);
void other(void);
void replace(unsigned char *?owned(free) *pp);
void keep_zone(struct zone *z);
struct zone { unsigned char *?owned(free) file; const unsigned char *?borrows(file) trans; };
inout struct zone *?borrows(static) find(int k);
struct zone *?owned make(void);
static struct zone arr[40];
static unsigned char *?owned(free) zfile;
static const unsigned char *?borrows(zfile) ztrans;
static unsigned char *?owned(free) *?keep;
"""
XPRELUDE_LINES = XPRELUDE.count("\n")


def _xdiags(tmp_path, body, *flags):
    proc = _compile(tmp_path, body, *flags, prelude=XPRELUDE)
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        where = parts[0].rsplit(":", 1)
        if len(parts) == 3 and parts[1] in ("warning", "error") and len(where) == 2 and where[1].isdigit():
            at = int(where[1]) - XPRELUDE_LINES
            if at > 0 and "defined but not used" not in parts[2]:
                out.append((at, parts[2]))
    return out


def _x_call(b, o, line, what):
    return (f"'{b}' borrows from '{o}', which was freed or changed at line {line + XPRELUDE_LINES}: set '{b}' "
            f"again (or to nullptr) before {what}")


X_CLEAN = {
    "detached_first": "void d(void)\n{\n  ztrans = nullptr;\n  free(move(zfile));\n  zfile = nullptr;\n  other();\n}\n",
    "index_reset": ("void r(int i)\n{\n  free(move(arr[i].file));\n  arr[i].file = nullptr;\n  arr[i].trans = nullptr;\n"
                    "}\n"),
    "index_plus_one": ("void r(int i)\n{\n  free(move(arr[i + 1].file));\n  arr[i + 1].file = nullptr;\n"
                       "  arr[i + 1].trans = nullptr;\n}\n"),
    "index_loop": ("void r(int n)\n{\n  for (int i = 0; i < n; i++)\n  {\n    free(move(arr[i].file));\n"
                   "    arr[i].file = nullptr;\n    arr[i].trans = nullptr;\n  }\n}\n"),
    "address_after_detach": "void r(void)\n{\n  ztrans = nullptr;\n  replace(&zfile);\n}\n",
    "local_pointer_reset": ("void r(void)\n{\n  struct zone *?z = find(3);\n  if (!z)\n    return;\n  free(move(z->file));\n"
                            "  z->file = nullptr;\n  z->trans = nullptr;\n}\n"),
    "pointer_to_a_local": ("int r(void)\n{\n  struct zone l = {nullptr, nullptr};\n  struct zone *p = &l;\n"
                           "  p->file = malloc(4);\n  if (!p->file)\n    return 0;\n  p->trans = p->file;\n"
                           "  free(move(p->file));\n  p->file = nullptr;\n  return 1;\n}\n"),
    "owned_freed_whole": ("void r(void)\n{\n  struct zone *?owned z = make();\n  if (!z)\n    return;\n"
                          "  free(move(z->file));\n  z->file = nullptr;\n  free(move(z));\n}\n"),
    "param_index_reset": ("void r(inout struct zone *zs, int i)\n{\n  free(move(zs[i].file));\n  zs[i].file = nullptr;\n"
                          "  zs[i].trans = nullptr;\n}\n"),
    "member_detached_first": ("void r(inout struct zone *z)\n{\n  z->trans = nullptr;\n  free(move(z->file));\n"
                              "  z->file = nullptr;\n}\n"),
}


@pytest.mark.parametrize("name", sorted(X_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_limits_clean(tmp_path, name, opt):
    assert _xdiags(tmp_path, X_CLEAN[name], opt) == []


X_BAD = {
    "index_left_dead": (
        "void r(int i)\n{\n  free(move(arr[i].file));\n  arr[i].file = nullptr;\n}\n",
        [(3, _dead_exit("arr[i].trans", "arr[i].file", "r"))],
    ),
    "index_changed_while_dead": (
        "void r(int i)\n{\n  free(move(arr[i].file));\n  arr[i].file = nullptr;\n  i = i + 1;\n  arr[i].trans = nullptr;\n}\n",
        [(3, _dead_exit("arr[i].trans", "arr[i].file", "r"))],
    ),
    "another_index_does_not_revive": (
        "void r(int i, int j)\n{\n  free(move(arr[i].file));\n  arr[i].file = nullptr;\n  arr[j].trans = nullptr;\n}\n",
        [(3, _dead_exit("arr[i].trans", "arr[i].file", "r"))],
    ),
    "past_sixteen": (
        "void r(void)\n{\n  free(move(arr[30].file));\n  arr[30].file = nullptr;\n}\n",
        [(3, _dead_exit("arr[30].trans", "arr[30].file", "r"))],
    ),
    "through_a_pointer": (
        "void r(void)\n{\n  unsigned char *?owned(free) *pp = &zfile;\n  free(move(*pp));\n  *pp = nullptr;\n}\n",
        [(4, _dead_exit("ztrans", "zfile", "r"))],
    ),
    "address_to_a_call": (
        "void r(void)\n{\n  replace(&zfile);\n  ztrans = nullptr;\n}\n",
        [(3, "'ztrans' points into 'zfile', whose address goes to 'replace': set it to nullptr (or static storage) "
             "first")],
    ),
    "address_stored": (
        "void r(void)\n{\n  keep = &zfile;\n}\n",
        [(3, "the address of 'zfile' is stored where it outlives the call: 'ztrans', which borrows from it, can no "
             "longer be followed")],
    ),
    "local_pointer_left_dead": (
        "void r(void)\n{\n  struct zone *?z = find(3);\n  if (!z)\n    return;\n  free(move(z->file));\n"
        "  z->file = nullptr;\n}\n",
        [(6, _dead_exit("z->trans", "z->file", "r"))],
    ),
    "owned_returned": (
        "struct zone *?owned r(void)\n{\n  struct zone *?owned z = make();\n  if (!z)\n    return nullptr;\n"
        "  free(move(z->file));\n  z->file = nullptr;\n  return z;\n}\n",
        [(8, _x_call("z->trans", "z->file", 6, "returning 'z'"))],
    ),
    "owned_handed_on": (
        "void r(void)\n{\n  struct zone *?owned z = make();\n  if (!z)\n    return;\n  free(move(z->file));\n"
        "  z->file = nullptr;\n  keep_zone(z);\n  free(move(z));\n}\n",
        [(8, _x_call("z->trans", "z->file", 6, "calling 'keep_zone'"))],
    ),
    "param_index_left_dead": (
        "void r(inout struct zone *zs, int i)\n{\n  free(move(zs[i].file));\n  zs[i].file = nullptr;\n}\n",
        [(3, _dead_exit("zs[i].trans", "zs[i].file", "r"))],
    ),
    "base_changed_while_dead": (
        "void r(inout struct zone *z, inout struct zone *w)\n{\n  free(move(z->file));\n  z->file = nullptr;\n"
        "  z = w;\n  z->trans = nullptr;\n}\n",
        [(3, _dead_exit("z->trans", "z->file", "r"))],
    ),
}


@pytest.mark.parametrize("name", sorted(X_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_limits_reported(tmp_path, name, opt):
    body, want = X_BAD[name]
    got = _xdiags(tmp_path, body, opt)
    for d in want:
        assert d in got, got


# --- objects through a loaded pointer, an object handed to any call, statics ---

YPRELUDE = XPRELUDE + """\
void *memset(void *s, int c, unsigned long n);
struct outer { int k; struct zone *in; };
"""


def _ydiags(tmp_path, body, *flags):
    proc = _compile(tmp_path, body, *flags, prelude=YPRELUDE)
    lines = YPRELUDE.count("\n")
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        where = parts[0].rsplit(":", 1)
        if len(parts) == 3 and parts[1] == "error" and len(where) == 2 and where[1].isdigit():
            at = int(where[1]) - lines
            if at > 0 and "borrows" in parts[2]:
                out.append((at, parts[2]))
    return out


Y_CLEAN = {
    "path_reset": ("void r(inout struct outer *o)\n{\n  free(move(o->in->file));\n  o->in->file = nullptr;\n"
                   "  o->in->trans = nullptr;\n}\n"),
    "local_static_from_global": ("void r(void)\n{\n  static const unsigned char *?borrows(zfile) cur;\n"
                                 "  if (zfile)\n    cur = zfile + 1;\n  if (cur)\n    other();\n}\n"),
    "local_static_owner": ("void r(void)\n{\n  static unsigned char *?owned(free) buf;\n"
                           "  static const unsigned char *?borrows(buf) at;\n  at = nullptr;\n  free(move(buf));\n"
                           "  buf = malloc(8);\n  at = buf;\n}\n"),
}


@pytest.mark.parametrize("name", sorted(Y_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_more_clean(tmp_path, name, opt):
    assert _ydiags(tmp_path, Y_CLEAN[name], opt) == []


Y_BAD = {
    "path_left_dead": (
        "void r(inout struct outer *o)\n{\n  free(move(o->in->file));\n  o->in->file = nullptr;\n}\n",
        [(3, _dead_exit("o->in->trans", "o->in->file", "r"))],
    ),
    "path_changed_while_dead": (
        "void r(inout struct outer *o, inout struct zone *?borrows(static) w)\n{\n  free(move(o->in->file));\n  o->in->file = nullptr;\n"
        "  o->in = w;\n  o->in->trans = nullptr;\n}\n",
        [(3, _dead_exit("o->in->trans", "o->in->file", "r"))],
    ),
    "object_to_any_call": (
        "void r(void)\n{\n  struct zone *?z = find(1);\n  if (!z)\n    return;\n  free(move(z->file));\n"
        "  z->file = nullptr;\n  memset(z, 0, 1);\n  z->trans = nullptr;\n}\n",
        [(8, f"'z->trans' borrows from 'z->file', which was freed or changed at line {6 + YPRELUDE.count(chr(10))}: set "
             "'z->trans' again (or to nullptr) before calling 'memset'")],
    ),
    "local_static_left_dead": (
        "void r(void)\n{\n  static const unsigned char *?borrows(zfile) cur;\n  free(move(zfile));\n"
        "  zfile = nullptr;\n  (void)cur;\n}\n",
        [(4, _dead_exit("cur", "zfile", "r"))],
    ),
}


@pytest.mark.parametrize("name", sorted(Y_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_more_reported(tmp_path, name, opt):
    body, want = Y_BAD[name]
    got = _ydiags(tmp_path, body, opt)
    for d in want:
        assert d in got, got


# --- a pointer loaded at a variable index (`o->in[i]`), aliases coming back ---

ZPRELUDE = """\
#pragma yasc
void free(drop void *?owned p);
struct zone { unsigned char *?owned(free) file; const unsigned char *?borrows(file) trans; };
struct outer { int k; struct zone **in; };
static struct zone arr[40];
"""
NOT_ONE_VARIABLE = "an element at an index that is not one variable is never the same twice: put the index in a variable"


def _zdiags(tmp_path, body, *flags):
    proc = _compile(tmp_path, body, *flags, prelude=ZPRELUDE)
    lines = ZPRELUDE.count("\n")
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        where = parts[0].rsplit(":", 1)
        if len(parts) == 3 and parts[1] in ("error", "note") and len(where) == 2 and where[1].isdigit():
            at = int(where[1]) - lines
            if at > 0 and ("borrows" in parts[2] or "index" in parts[2]):
                out.append((at, parts[2]))
    return out


Z_CLEAN = {
    "path_index_reset": ("void r(inout struct outer *o, int i)\n{\n  free(move(o->in[i]->file));\n"
                         "  o->in[i]->file = nullptr;\n  o->in[i]->trans = nullptr;\n}\n"),
    "global_index_alias_comes_back": ("void r(int i, int j)\n{\n  free(move(arr[i].file));\n  arr[i].file = nullptr;\n"
                                      "  arr[i].trans = nullptr;\n  if (arr[j].trans)\n    arr[j].trans = nullptr;\n}\n"),
}


@pytest.mark.parametrize("name", sorted(Z_CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_index_clean(tmp_path, name, opt):
    assert _zdiags(tmp_path, Z_CLEAN[name], opt) == []


Z_BAD = {
    "path_index_left_dead": (
        "void r(inout struct outer *o, int i)\n{\n  free(move(o->in[i]->file));\n  o->in[i]->file = nullptr;\n}\n",
        [(3, _dead_exit("o->in[i]->trans", "o->in[i]->file", "r"))],
    ),
    "path_other_index": (
        "void r(inout struct outer *o, int i, int j)\n{\n  free(move(o->in[i]->file));\n  o->in[i]->file = nullptr;\n"
        "  o->in[j]->trans = nullptr;\n}\n",
        [(3, _dead_exit("o->in[i]->trans", "o->in[i]->file", "r"))],
    ),
    "path_index_changed": (
        "void r(inout struct outer *o, int i)\n{\n  free(move(o->in[i]->file));\n  o->in[i]->file = nullptr;\n  i++;\n"
        "  o->in[i]->trans = nullptr;\n}\n",
        [(3, _dead_exit("o->in[i]->trans", "o->in[i]->file", "r"))],
    ),
    "path_index_not_one_variable": (
        "void r(inout struct outer *o, int i, int k)\n{\n  free(move(o->in[i * k]->file));\n"
        "  o->in[i * k]->file = nullptr;\n  o->in[i * k]->trans = nullptr;\n}\n",
        [(3, _dead_exit("o->in[?]->trans", "o->in[?]->file", "r")), (3, NOT_ONE_VARIABLE)],
    ),
    "global_index_not_one_variable": (
        "void r(int i, int k)\n{\n  free(move(arr[i * k].file));\n  arr[i * k].file = nullptr;\n"
        "  arr[i * k].trans = nullptr;\n}\n",
        [(3, _dead_exit("arr[?].trans", "arr[?].file", "r")), (3, NOT_ONE_VARIABLE)],
    ),
}


@pytest.mark.parametrize("name", sorted(Z_BAD))
@pytest.mark.parametrize("opt", LEVELS)
def test_index_reported(tmp_path, name, opt):
    body, want = Z_BAD[name]
    got = _zdiags(tmp_path, body, opt)
    for d in want:
        assert d in got, got


@pytest.mark.parametrize("opt", LEVELS)
def test_aliased_read_said_once_and_nothing_at_the_way_out(tmp_path, opt):
    """arr[j] may be the freed arr[i]: the read is said, once, by its own name;
    once arr[i].trans is set again, arr[j] is either it or untouched."""
    body = ("int r(int i, int j)\n{\n  free(move(arr[i].file));\n  arr[i].file = nullptr;\n"
            "  int v = arr[j].trans ? 1 : 0;\n  arr[i].trans = nullptr;\n  return v;\n}\n")
    lines = ZPRELUDE.count("\n")
    got = _zdiags(tmp_path, body, opt)
    assert got == [(5, f"'arr[j].trans' may be used after 'arr[i].file', which it borrows from, was freed or changed "
                       f"(line {3 + lines})")], got
    body2 = ("int r(inout struct outer *o, int i, int j)\n{\n  free(move(o->in[i]->file));\n  o->in[i]->file = nullptr;\n"
             "  int v = o->in[j]->trans ? 1 : 0;\n  o->in[i]->trans = nullptr;\n  return v;\n}\n")
    got2 = _zdiags(tmp_path, body2, opt)
    assert [d for d in got2 if "returns" in d[1]] == [], got2
    assert any("may be used after" in d[1] for d in got2), got2
