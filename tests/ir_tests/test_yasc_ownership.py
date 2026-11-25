"""The yasc dialect: ownership keywords and the rules the types alone decide.

`-std=yasc` (C23 plus the dialect, for the main file) or `#pragma yasc` (from
there to the end of its file) make `owned view maybe out drop` keywords;
elsewhere they are identifiers.  `owned`, `view` and `maybe` qualify a pointer
(after the `*`), `out` and `drop` the object (before the type).  The qualifiers never change the
generated code.  The flow checks (moves, leaks at scope exit, double free)
are -Wlifetime's, in test_lifetime.py; these are the type-level rules:
  - a pointer that owns nothing stored in an owned one,
  - an owned result stored in a pointer that owns nothing (never freed),
  - an owned result discarded.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

NOT_OWNING = "a pointer that owns nothing is stored in an owned pointer"
LEAK = "an owned result is stored in a pointer that owns nothing: it is never freed"
DISCARD = "an owned result is discarded: it is never freed"

PRELUDE = """\
void * owned maybe alloc(unsigned n);
void release(drop void * owned maybe p);
char * owned make(void);
char * borrows(static) plain(void);
"""


def _compile(tmp_path, body, *flags, name="t.c", obj="t.o"):
    src = tmp_path / name
    src.write_text(body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-cross-tu", "-Wno-tainted-format", "-c", *flags, str(src), "-o", str(tmp_path / obj)],
        capture_output=True, text=True,
    )


def _yasc(tmp_path, body, *flags):
    # the flow checks (-Wlifetime, on by default) are test_lifetime.py's
    return _compile(tmp_path, PRELUDE + body, "-ccompat", "-std=yasc", "-Wno-lifetime", *flags)


def _warnings(proc):
    return [line.split("warning: ", 1)[1] for line in proc.stderr.splitlines() if "warning: " in line]


def test_keywords_are_identifiers_outside_yasc(tmp_path):
    body = "int owned = 1, view = 2, maybe, out, drop;\nint f(char *out) { return owned + view + !out; }\n"
    for std in ("-std=c11", "-std=c23"):
        proc = _compile(tmp_path, body, std)
        assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_yasc_macro(tmp_path):
    body = "#ifdef __YASC__\nint yes = __YASC__;\n#else\n#error no __YASC__\n#endif\n"
    assert _compile(tmp_path, body, "-std=yasc").returncode == 0
    assert _compile(tmp_path, body, "-std=c23").returncode != 0
    proc = _compile(tmp_path, "#pragma yasc\n" + body, "-std=c23")
    assert proc.returncode == 0, proc.stderr


def test_pragma_starts_the_dialect(tmp_path):
    body = "int out;\n#pragma yasc\n" + PRELUDE + "void f(void) { char * view v = make(); (void)v; }\n"
    proc = _compile(tmp_path, body, "-std=c23")
    assert proc.returncode == 0, proc.stderr
    assert _warnings(proc) == [LEAK]


def test_correct_ownership_is_silent(tmp_path):
    proc = _yasc(tmp_path, """\
struct S { char * owned text; char * view peek; };
typedef char * owned ostr;
char * owned pass(char * owned p) { return p; }
void take(drop struct S * view s);
int fill(out struct S * view s);
void f(struct S * view s);
void ok(inout struct S * view s)
{
  char * owned p = alloc(4);
  char * owned q = 0;
  char * view borrow = p;
  char * view inside = p + 1;
  ostr t = make();
  s->text = make();
  s->peek = s->text;
  q = pass(move(t));
  release(p);
  release(q);
  release(alloc(1));
  (void)borrow, (void)inside;
}
""")
    assert proc.returncode == 0, proc.stderr
    assert _warnings(proc) == []


@pytest.mark.parametrize(
    "body, expected",
    [
        ("void f(inout char * view q) { char * owned p = q; release(p); }", [NOT_OWNING]),
        ("void f(inout char * view q) { release(q); }", [NOT_OWNING]),
        ("char * owned f(inout char * view q) { return q; }", [NOT_OWNING]),
        ("void f(void) { char * owned p = plain(); release(p); }", [NOT_OWNING]),
        ("typedef char * owned ostr;\nvoid f(inout char * view q) { ostr s = q; release(s); }", [NOT_OWNING]),
        ("struct S { char * owned t; };\nvoid f(inout struct S * view s, inout char * view q) { s->t = q; }", [NOT_OWNING]),
        ("void f(inout char * owned * view pp, inout char * view q) { *pp = q; }", [NOT_OWNING]),
        ("void f(void) { char * view v = make(); (void)v; }", [LEAK]),
        ("void use(char * view s);\nvoid f(void) { use(make()); }", [LEAK]),
        ("char * borrows(static) f(void) { return make(); }", [LEAK]),
        ("void f(void) { make(); }", [DISCARD]),
        ("void f(void) { alloc(8), make(); }", [DISCARD, DISCARD]),
        ("void f(void) { for (make();; make()) break; }", [DISCARD, DISCARD]),
    ],
)
def test_ownership_warnings(tmp_path, body, expected):
    proc = _yasc(tmp_path, body + "\n")
    assert proc.returncode == 0, proc.stderr
    assert _warnings(proc) == expected


@pytest.mark.parametrize(
    "body, message",
    [
        ("owned char * view p;", "'owned' qualifies a pointer: write it after the '*'"),
        ("maybe int * view p;", "'maybe' qualifies a pointer: write it after the '*'"),
        ("view int * view p;", "'view' qualifies a pointer: write it after the '*'"),
        ("char * owned view p;", "a pointer is owned, a view or borrows(...): one of them"),
        ("char * out p;", "'out' qualifies the object: write it before the type"),
        ("char * drop p;", "'drop' qualifies the object: write it before the type"),
    ],
)
def test_misplaced_qualifiers(tmp_path, body, message):
    proc = _yasc(tmp_path, body + "\n")
    assert proc.returncode != 0 and message in proc.stderr, proc.stderr


def test_type_names_show_pointer_qualifiers(tmp_path):
    proc = _yasc(tmp_path, "struct S { int a; };\nvoid f(void) { struct S s; s = make(); }\n")
    assert proc.returncode != 0
    assert "'char * owned'" in proc.stderr, proc.stderr


YASC_FILE = PRELUDE + """\
struct node { struct node * owned maybe next; char * owned name; };
int fill(out struct node * view n) { n->next = 0; n->name = make(); return 0; }
void drain(drop struct node * view n)
{
  while (n->next) {
    struct node * owned maybe next = move(n->next->next);
    release(n->next->name);
    release(n->next);
    n->next = move(next);
  }
  release(n->name);
}
"""


@pytest.mark.parametrize("level", ["-O0", "-O1", "-O2", "-Os"])
def test_qualifiers_do_not_change_code(tmp_path, level):
    gone = ["-Downed=", "-Dview=", "-Dmaybe=", "-Dout=", "-Ddrop=", "-Dborrows(...)=", "-Dmove(x)=(x)"]
    yasc = _compile(tmp_path, YASC_FILE, "-std=yasc", level, name="y.c", obj="y.o")
    # -std=yasc is safe mode, so -Wlifetime (the SSA path at -O0) is on there
    plain = _compile(tmp_path, YASC_FILE, "-std=c23", "-Wlifetime", level, *gone, name="p.c", obj="p.o")
    assert yasc.returncode == 0 and plain.returncode == 0, yasc.stderr + plain.stderr
    y, p = (tmp_path / "y.o").read_bytes(), (tmp_path / "p.o").read_bytes()
    # The objects name their source file; compare everything else.
    assert len(y) == len(p) and y.replace(b"y.c", b"p.c") == p


# ---------------------------------------------------------------------------
# Regular C seen from yasc: unchecked pointers (docs/yasc_modules.md, "Legacy C")
#
# A file is yasc when it is the main file of a yasc unit, says #pragma yasc, or
# is an imported interface; an #include starts in regular C.  A pointer written
# in regular C -- a header, or a macro defined in one -- says nothing about
# ownership: it converts to and from owned and view pointers silently, it is
# neither maybe nor null, and the checks start at the yasc variable it lands in.

LEGACY_H = """\
void *c_alloc(unsigned n);
char *c_name(void);
void c_take(char *p);
typedef struct c_file C_FILE;
C_FILE *c_open(const char *path);
char **c_list(void);
#define C_MK(n) ((char *)c_alloc(n))
"""

YASC_H = """\
#pragma yasc
char * borrows(static) y_hdr_view(void);
"""

LEGACY_USER = """\
#include "legacy.h"
#include "yasc.h"
char * owned make(void);
char * borrows(static) y_view(void);
void y_take(char * view p);
void release(drop void * owned maybe p);
#define Y_MK(n) ((char * view)c_alloc(n))
void f(void)
{
  char * owned a = c_alloc(4);
  char * owned b = c_name();
  C_FILE * owned fp = c_open("x");
  char * owned c = C_MK(3);
  char * owned * owned l = c_list();
  c_take(make());
  char * owned d = Y_MK(3);
  char * owned e = y_view();
  char * owned g = y_hdr_view();
  y_take(make());
  release(a); release(b); release(fp); release(c); release(l);
  release(d); release(e); release(g);
}
"""


def _located(proc):
    return [(int(l.split(":")[1]), l.split("warning: ", 1)[1]) for l in proc.stderr.splitlines() if "warning: " in l]


def test_legacy_c_pointers_are_unchecked(tmp_path):
    (tmp_path / "legacy.h").write_text(LEGACY_H)
    (tmp_path / "yasc.h").write_text(YASC_H)
    # -ccompat: the type rules below stay warnings (errors in safe mode)
    proc = _compile(tmp_path, LEGACY_USER, "-ccompat", "-std=yasc")
    assert proc.returncode == 0, proc.stderr
    # silent: C functions, a C typedef, a C macro's cast, pointers to pointers,
    # an owned result handed to C; checked: a yasc macro's cast, a yasc view,
    # a #pragma yasc header, an owned result handed to yasc
    assert _located(proc) == [(16, NOT_OWNING), (17, NOT_OWNING), (18, NOT_OWNING), (19, LEAK)], proc.stderr


def test_legacy_c_in_safe_mode(tmp_path):
    # safe by default (no -ccompat) is yasc too
    (tmp_path / "legacy.h").write_text(LEGACY_H)
    body = ('#include "legacy.h"\nvoid release(drop void * owned maybe p);\n'
            "void f(void)\n{\n  char * owned a = unsafe(c_alloc(4));\n  release(a);\n}\n")
    proc = _compile(tmp_path, body)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_string_literals_do_not_end_checks(tmp_path):
    # a token string replayed for a string literal must not leave the parser
    # reading the rest of the unit as regular C
    body = 'const char * view s = "x";\nvoid f(void) { char * owned e = plain(); release(e); (void)s; }\n'
    assert _warnings(_yasc(tmp_path, body)) == [NOT_OWNING]


def test_plain_c_unit_unaffected(tmp_path):
    (tmp_path / "legacy.h").write_text(LEGACY_H)
    body = '#include "legacy.h"\nchar *f(void) { char *owned = c_alloc(1); char *view = C_MK(2); return owned ? owned : view; }\n'
    proc = _compile(tmp_path, body, "-std=c11")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


# GCC attributes refine regular C: malloc is an owned result (owned maybe;
# owned with returns_nonnull), malloc(dealloc, i) makes the deallocator's
# argument owned (and drop), nonnull is checked at calls (-Wnonnull, in -Wall).

ATTR_H = """\
typedef unsigned size_t;
void my_free(void *p);
__attribute__((malloc, malloc(my_free, 1))) void *my_alloc(size_t n);
char *my_dup(const char *s) __attribute__((__malloc__)) __attribute__((__nonnull__(1)));
void *xalloc(size_t n) __attribute__((malloc, returns_nonnull));
char *my_cpy(char *d, const char *s) __attribute__((nonnull));
char *getname(void);
"""

ATTR_USER = """\
#include "attr.h"
char * borrows(static) y_view(void);
void f(void)
{
  char * owned maybe a = my_alloc(4);
  char * owned maybe c = my_dup("x");
  char * owned d = getname();
  char * owned e = xalloc(1);
  my_free(a);
  my_free(getname());
  my_free(0);
  char * view b = my_alloc(4);
  my_alloc(8);
  my_free(y_view());
  my_dup(0);
  if (c)
    my_cpy(c, 0);
  char * owned z = my_alloc(1);
  my_free(c); my_free(d); my_free(e); my_free(z);
  (void)b;
}
"""

NONNULL1 = "null passed as argument 1, which is declared nonnull"
NONNULL2 = "null passed as argument 2, which is declared nonnull"
MAYBE_INTO_Z = "a pointer that may be null is stored in 'z', which is not maybe"


def test_attributes_refine_regular_c(tmp_path):
    (tmp_path / "attr.h").write_text(ATTR_H)
    # -ccompat: the flow check's report below is a warning there (an error
    # in safe mode, strict lifetimes)
    proc = _compile(tmp_path, ATTR_USER, "-ccompat", "-std=yasc", "-Wall", "-Wlifetime")
    assert proc.returncode == 0, proc.stderr
    warnings = _located(proc)
    # silent: owned results into owned maybe pointers, an unchecked one, a
    # returns_nonnull result into a pointer that is not maybe, owned and
    # unchecked pointers and null into the deallocator
    for expected in [
        (12, LEAK),  # an owned result into a view
        (13, DISCARD),
        (14, NOT_OWNING),  # a view into the deallocator
        (15, NONNULL1),
        (15, DISCARD),
        (17, NONNULL2),
    ]:
        assert expected in warnings, proc.stderr
    # malloc's result may be null (the flow checks, -Wlifetime)
    assert any(line == 18 and msg.endswith(MAYBE_INTO_Z) for line, msg in warnings), proc.stderr
    assert all(line >= 12 for line, _ in warnings), proc.stderr


def test_nonnull_in_plain_c(tmp_path):
    (tmp_path / "attr.h").write_text(ATTR_H)
    body = '#include "attr.h"\nvoid g(void) { char *p = my_alloc(3); my_free(p); my_dup(0); my_dup(p); }\n'
    proc = _compile(tmp_path, body, "-std=c11", "-Wall")
    assert _warnings(proc) == [NONNULL1], proc.stderr  # nothing about ownership in C
    proc = _compile(tmp_path, body, "-std=c11")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr  # -Wnonnull is in -Wall
    proc = _compile(tmp_path, body, "-std=c11", "-Wall", "-Wno-nonnull")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


NONNULL_REDECL = """\
int fat(int fd, const char *path, int fl);
int fat(int fd, const char *path, int fl) __attribute__((nonnull(2)));
int fat(int fd, const char *path, int fl) { return fd + (path != 0) + fl; }
int two(const char *a, const char *b) __attribute__((nonnull(1)));
int two(const char *a, const char *b) __attribute__((nonnull(2)));
int all(const char *a, int n, const char *b);
int all(const char *a, int n, const char *b) __attribute__((nonnull));
int def(const char *a, const char *b) __attribute__((nonnull(2)));
int def(const char *a, const char *b) { return a == b; }
int late(const char *a, const char *b) { return a == b; }
int late(const char *a, const char *b) __attribute__((nonnull(1)));
int none(const char *a, const char *b);
int none(const char *a, const char *b);
typedef int fn_t(const char *a);
fn_t td;
int td(const char *a) __attribute__((nonnull));
fn_t other;
int use(void)
{
  return fat(1, 0, 0) + two(0, 0) + all(0, 0, 0) + def(0, 0) + late(0, 0) + none(0, 0) + td(0) + other(0);
}
"""


def test_nonnull_merges_across_declarations(tmp_path):
    # as gcc: every declaration's nonnull counts, a list ORs into the others,
    # and nonnull without a list covers every pointer parameter
    proc = _compile(tmp_path, NONNULL_REDECL, "-std=c11", "-Wall")
    assert proc.returncode == 0, proc.stderr
    got = sorted(int(msg.split("argument ")[1].split(",")[0]) for msg in _warnings(proc))
    # fat 2; two 1, 2; all 1, 3; def 2; late 1; td 1 (the typedef's own type
    # is shared: `other` stays without)
    assert got == sorted([2, 1, 2, 1, 3, 2, 1, 1]), proc.stderr
    assert all(msg.endswith("which is declared nonnull") for msg in _warnings(proc)), proc.stderr


def test_has_attribute_reports_them(tmp_path):
    body = ("#if __has_attribute(malloc) && __has_attribute(__nonnull__) && __has_attribute(returns_nonnull)\n"
            "int ok;\n#else\n#error missing\n#endif\n")
    proc = _compile(tmp_path, body, "-std=c11")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_unknown_deallocator(tmp_path):
    proc = _compile(tmp_path, "__attribute__((malloc(nosuch, 1))) void * owned maybe a(unsigned);\n", "-std=yasc")
    assert _warnings(proc) == ["malloc: deallocator 'nosuch' is not a function declared before"], proc.stderr
