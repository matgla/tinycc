"""Fix-its decided from the code (driver/diag_out.c, builtin/objsize.c,
decl/plain_param.c).

1. sprintf(buf, ...) -> snprintf(buf, sizeof buf, ...), vsprintf -> vsnprintf,
   strcpy -> strlcpy, strcat -> strlcat, where the destination's size is
   known exactly (a declared array, a member, a constant allocation):
   `sizeof buf` when the destination is written as a char array's name, else
   the size as a constant.  Carried by the first diagnostic of the call: the
   deprecated warning (the call's report waits for its arguments), else
   -Wformat-overflow / -Wstringop-overflow.  The fix-its need the name and the
   arguments up to the one the size follows on the call's line; otherwise only
   the note.
2. yasc's "plain pointer in yasc: say owned or view for 'p'" on a
   definition's parameter is given at the end of the body, with `* owned`
   when the body frees it, passes it to an owned or drop parameter, keeps it
   where it outlives the call or returns it as owned, else `* view`; `maybe`
   as well when it is tested for null or passed to a maybe parameter.  A
   prototype keeps `* view`.  A local's own pointer the same way (owned also
   when it is given an owned value; maybe also when it is given null or a
   maybe value), and an alias (`q = p`) carries its uses to what it copies.

Each fix-it is applied (test_diag_format._apply) and the result compiled.
"""

import json
import subprocess
from pathlib import Path

import pytest

from cross_includes import INCLUDE_FLAGS

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

C_DECLS = """\
#include <stdarg.h>
int sprintf(char *, const char *, ...) __attribute__((deprecated));
int vsprintf(char *, const char *, va_list) __attribute__((deprecated));
char *strcpy(char *, const char *) __attribute__((deprecated));
char *strcat(char *, const char *) __attribute__((deprecated));
int snprintf(char *, __SIZE_TYPE__, const char *, ...);
int vsnprintf(char *, __SIZE_TYPE__, const char *, va_list);
__SIZE_TYPE__ strlcpy(char *, const char *, __SIZE_TYPE__);
__SIZE_TYPE__ strlcat(char *, const char *, __SIZE_TYPE__);
void *malloc(__SIZE_TYPE__);
struct S { int a; char name[12]; };
char buf[16];
"""

DEP_SRC = (
    C_DECLS
    + """\
void f(const char *s, int x, va_list ap)
{
  char loc[32];
  char *p = malloc(64);
  struct S st;
  sprintf(buf, "%d", x);
  sprintf(loc, "%s", s);
  sprintf(p, "%s", s);
  vsprintf(loc, s, ap);
  strcpy(loc, s);
  strcat(buf + 2, s);
  strcpy(st.name, s);
  sprintf(buf,
          "%d", x);
  (void)st;
}
"""
)


def _line(src, text):
    return [i + 1 for i, l in enumerate(src.splitlines()) if text in l][0]


def _line_eq(src, text):
    return src.splitlines().index(text) + 1


def _write(tmp_path, name, src):
    p = tmp_path / name
    p.write_text(src)
    return p


def _tcc(tmp_path, *args):
    return subprocess.run([str(TCC), "-Wno-missing-prototypes", *INCLUDE_FLAGS, *map(str, args)], capture_output=True, text=True, cwd=tmp_path)


def _json(tmp_path, src, *flags, name="t.c"):
    _write(tmp_path, name, src)
    proc = _tcc(tmp_path, "-c", *flags, "-fdiagnostics-format=json", "-fmax-errors=0", name, "-o", "t.o")
    return proc, json.loads(proc.stderr)


def _apply(src, fixits):
    """apply gcc-style JSON fix-its (one line each), right to left"""
    lines = src.splitlines(keepends=True)
    for f in sorted(fixits, key=lambda f: (f["start"]["line"], f["start"]["column"]), reverse=True):
        ln = f["start"]["line"] - 1
        a, b = f["start"]["column"] - 1, f["next"]["column"] - 1
        lines[ln] = lines[ln][:a] + f["string"] + lines[ln][b:]
    return "".join(lines)


def _all_fixits(diags):
    return [f for d in diags for f in d.get("fixits", [])]


# ------------------------------------------------------- sprintf -> snprintf


def test_bound_fixits_text(tmp_path):
    _write(tmp_path, "t.c", DEP_SRC)
    proc = _tcc(tmp_path, "-ccompat", "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o", "t.o")
    lines = proc.stderr.splitlines()
    src = DEP_SRC
    ln = _line(src, 'sprintf(buf, "%d", x);')
    assert f"t.c:{ln}: warning: 'sprintf' is deprecated" in lines
    i = lines.index(f"t.c:{ln}: warning: 'sprintf' is deprecated")
    # the warning, its note, the fix-its, then "declared here"
    assert lines[i + 1] == f"t.c:{ln}: note: use snprintf(buf, sizeof buf, ...), which bounds the output"
    assert lines[i + 2] == f'fix-it:"t.c":{{{ln}:3-{ln}:10}}:"snprintf"'
    assert lines[i + 3] == f'fix-it:"t.c":{{{ln}:14-{ln}:14}}:", sizeof buf"'
    assert lines[i + 4].endswith("note: declared here")
    expect = {
        'sprintf(loc, "%s", s);': "use snprintf(loc, sizeof loc, ...), which bounds the output",
        'sprintf(p, "%s", s);': "use snprintf(p, 64, ...), which bounds the output",
        "vsprintf(loc, s, ap);": "use vsnprintf(loc, sizeof loc, fmt, ap), which bounds the output",
        "strcpy(loc, s);": "use strlcpy(loc, src, sizeof loc), which bounds the copy",
        # buf + 2: 14 bytes left, as a constant
        "strcat(buf + 2, s);": "use strlcat(buf + 2, src, 14), which bounds the copy",
        "strcpy(st.name, s);": "use strlcpy(st.name, src, 12), which bounds the copy",
    }
    for call, note in expect.items():
        assert f"t.c:{_line(src, call)}: note: {note}" in lines, (call, proc.stderr)
    # the first argument on the call's line is enough
    ln = _line_eq(src, "  sprintf(buf,")
    assert f'fix-it:"t.c":{{{ln}:14-{ln}:14}}:", sizeof buf"' in lines
    assert proc.stderr.count("fix-it:") == 16


def test_bound_fixits_apply(tmp_path):
    _, diags = _json(tmp_path, DEP_SRC, "-ccompat")
    fixed = _apply(DEP_SRC, _all_fixits(diags))
    assert '  snprintf(buf, sizeof buf, "%d", x);\n' in fixed
    assert '  snprintf(p, 64, "%s", s);\n' in fixed
    assert "  vsnprintf(loc, sizeof loc, s, ap);\n" in fixed
    assert "  strlcpy(loc, s, sizeof loc);\n" in fixed
    assert "  strlcat(buf + 2, s, 14);\n" in fixed
    assert "  strlcpy(st.name, s, 12);\n" in fixed
    _write(tmp_path, "fixed.c", fixed)
    proc = _tcc(tmp_path, "-ccompat", "-Wall", "-c", "fixed.c", "-o", "f.o")
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


def test_bound_fixit_sarif(tmp_path):
    _write(tmp_path, "t.c", DEP_SRC)
    proc = _tcc(tmp_path, "-ccompat", "-fdiagnostics-format=sarif", "-c", "t.c", "-o", "t.o")
    res = json.loads(proc.stderr)["runs"][0]["results"]
    ln = _line(DEP_SRC, "strcpy(loc, s);")
    r = [r for r in res if r["locations"][0]["physicalLocation"]["region"]["startLine"] == ln][0]
    fix = r["fixes"][0]
    assert fix["description"]["text"] == "use strlcpy(loc, src, sizeof loc), which bounds the copy"
    reps = fix["artifactChanges"][0]["replacements"]
    assert [x["insertedContent"]["text"] for x in reps] == ["strlcpy", ", sizeof loc"]


def test_bound_unknown_or_not_on_the_line(tmp_path):
    src = (
        C_DECLS
        + """\
#define SPR sprintf
void f(char *out, const char *s)
{
  sprintf(out, "%s", s);
  sprintf(
      buf, "%s", s);
  SPR(buf, "%s", s);
}
"""
    )
    _write(tmp_path, "t.c", src)
    proc = _tcc(tmp_path, "-ccompat", "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o", "t.o")
    lines = proc.stderr.splitlines()
    # a parameter: no size, the general note
    ln = _line(src, 'sprintf(out, "%s", s);')
    assert f"t.c:{ln}: note: use snprintf(buf, size, fmt, ...), which bounds the output" in lines
    # the size is known but the call is not on one line, or is a macro: a note
    for ln in (_line_eq(src, "  sprintf("), _line(src, "SPR(buf")):
        assert f"t.c:{ln}: note: use snprintf(dst, 16, ...), which bounds the output" in lines, proc.stderr
    assert "fix-it:" not in proc.stderr


def test_overflow_warnings_carry_it(tmp_path):
    src = """\
int sprintf(char *, const char *, ...);
char *strcpy(char *, const char *);
char *strcat(char *, const char *);
int snprintf(char *, __SIZE_TYPE__, const char *, ...);
__SIZE_TYPE__ strlcpy(char *, const char *, __SIZE_TYPE__);
__SIZE_TYPE__ strlcat(char *, const char *, __SIZE_TYPE__);
char g[4];
void f(void)
{
  char loc[8];
  sprintf(loc, "%s-%d", "abcdef", 12345);
  strcpy(g, "hello");
  strcat(loc, "0123456789");
}
"""
    proc, diags = _json(tmp_path, src, "-ccompat", "-Wall")
    opts = [d.get("option") for d in diags]
    assert opts == ["-Wformat-overflow", "-Wstringop-overflow", "-Wstringop-overflow"], proc.stderr
    assert [d["children"][0]["message"] for d in diags] == [
        "use snprintf(loc, sizeof loc, ...), which bounds the output",
        "use strlcpy(g, src, sizeof g), which bounds the copy",
        "use strlcat(loc, src, sizeof loc), which bounds the copy",
    ]
    fixed = _apply(src, _all_fixits(diags))
    assert '  snprintf(loc, sizeof loc, "%s-%d", "abcdef", 12345);\n' in fixed
    _write(tmp_path, "fixed.c", fixed)
    proc = _tcc(tmp_path, "-ccompat", "-Wall", "-c", "fixed.c", "-o", "f.o")
    # bounded now: what overflowed is truncated, and said so
    assert proc.returncode == 0 and "overflow" not in proc.stderr, proc.stderr
    proc = _tcc(tmp_path, "-ccompat", "-Wall", "-Wno-format-truncation", "-c", "fixed.c", "-o", "f.o")
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


def test_one_fixit_per_call(tmp_path):
    # deprecated and overflowing: the deprecated warning (first) carries it
    src = C_DECLS + 'void f(void) { char d[4]; strcpy(d, "hello"); }\n'
    proc, diags = _json(tmp_path, src, "-ccompat", "-Wall")
    assert [d.get("option") for d in diags] == ["-Wdeprecated-declarations", "-Wstringop-overflow"], proc.stderr
    assert len(diags[0]["fixits"]) == 2 and not diags[1].get("fixits")
    assert not diags[1].get("children")
    # without the deprecated warning, the overflow one does
    proc, diags = _json(tmp_path, src, "-ccompat", "-Wall", "-Wno-deprecated-declarations")
    assert [d.get("option") for d in diags] == ["-Wstringop-overflow"]
    assert len(diags[0]["fixits"]) == 2


def test_bound_same_at_every_level(tmp_path):
    _write(tmp_path, "t.c", DEP_SRC)
    outs = [
        _tcc(tmp_path, "-ccompat", "-Wall", o, "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o", "t.o").stderr
        for o in ("-O0", "-O2", "-fsyntax-only")
    ]
    assert outs[0] == outs[1] == outs[2]


def test_bound_pragma_and_wno(tmp_path):
    src = C_DECLS.replace(
        "char buf[16];\n",
        'char buf[16];\n#pragma GCC diagnostic ignored "-Wdeprecated-declarations"\n',
    ) + 'void f(void) { sprintf(buf, "%d", 1); }\n'
    _write(tmp_path, "t.c", src)
    proc = _tcc(tmp_path, "-ccompat", "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o", "t.o")
    assert proc.stderr == ""
    _write(tmp_path, "u.c", DEP_SRC)
    proc = _tcc(tmp_path, "-ccompat", "-Wno-deprecated-declarations", "-fdiagnostics-parseable-fixits", "-c", "u.c")
    assert proc.stderr == ""


def test_global_offset_object_size(tmp_path):
    # the offset of `buf + 10` folds into the symbol: 6 bytes are left
    src = """\
char *strcpy(char *, const char *);
char buf[16];
void f(void) { strcpy(buf + 10, "abcdefg"); }
"""
    _write(tmp_path, "t.c", src)
    proc = _tcc(tmp_path, "-ccompat", "-Wall", "-c", "t.c", "-o", "t.o")
    assert "t.c:3: warning: 'strcpy' writing 8 bytes into a region of size 6 overflows the destination" in proc.stderr
    assert "use strlcpy(buf + 10, src, 6)" in proc.stderr


# --------------------------------------------- owned or view, from the body

YASC_SRC = """\
void free(drop void * owned maybe p);
struct box { char * owned p; };
void take(char * owned p);
void peek(char * maybe p);
int borrow(char *s) { return s[0]; }
void freeit(char *s)
{
  free(s);
}
void put(inout struct box *b, char *s)
{
  b->p = s;
}
void pass(char *s)
{
  take(s);
}
char * owned ret(char *s)
{
  return s;
}
int test(char *s)
{
  if (!s)
    return 0;
  return s[0];
}
void mb(char *s)
{
  peek(s);
}
void both(char *s)
{
  if (s != nullptr)
    free(s);
}
int proto(char *s);
"""

# a parameter is a view unless it says owned: what needs it owned is an error
# at the use, with a note naming the parameter
YASC_OWN_LINES = [8, 12, 16, 20, 35]
YASC_PARAM_ERR = ("the parameter 's' owns nothing (a view unless it says owned): write '* owned s' when the "
                  "function takes it over, 'drop' before the type too when it frees it")


def test_default_view_param_body_says_owned(tmp_path):
    proc, diags = _json(tmp_path, YASC_SRC)
    assert not [d for d in diags if d["message"].startswith("plain pointer in yasc")], diags
    errs = [d for d in diags if d["message"] == YASC_PARAM_ERR]
    assert [d["locations"][0]["caret"]["line"] for d in errs] == YASC_OWN_LINES, errs
    # borrow, test (a null test of a view), mb (a view for a maybe parameter), proto: fine
    lines = {d["locations"][0]["caret"]["line"] for d in diags if d["kind"] == "error"}
    assert lines <= set(YASC_OWN_LINES), diags


def test_plain_param_error_order(tmp_path):
    # the parameter's error comes after the body's own diagnostics, before
    # the function's flow checks; each note and fix-it stays with its error
    # (a sizeof's plain pointer is said at once); the body's parameters and
    # locals in line order
    src = """\
int f(char *s)
{
  int z = sizeof(char *) == 4;
  char *loc = s;
  return loc[0] + z;
}
"""
    _write(tmp_path, "t.c", src)
    proc = _tcc(tmp_path, "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o", "t.o")
    # (s is a view: a parameter's own pointer says nothing; an `in` one, so
    # its target is const)
    assert proc.stderr.splitlines() == [
        "t.c:3: error: plain pointer in yasc: say owned or view for 'char *'",
        "t.c:4: error: assignment to 'char *' from 'const char * view' discards 'const' qualifier from pointer target type",
        "t.c:4: error: plain pointer in yasc: say owned or view for 'loc'",
        "t.c:4: note: the function only borrows through 'loc': write 'const * view'",
        'fix-it:"t.c":{4:8-4:9}:"const * view "',
    ]
    assert proc.returncode == 1


def test_plain_param_same_at_every_level(tmp_path):
    _write(tmp_path, "t.c", YASC_SRC)
    outs = [
        _tcc(tmp_path, o, "-fmax-errors=0", "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o", "t.o").stderr
        for o in ("-O0", "-O2", "-fsyntax-only")
    ]
    assert outs[0] == outs[1] == outs[2]
    # a static function's body may be compiled at the end of the unit (as its
    # other diagnostics are): the same reports, in another order
    src = "void free(drop void * owned maybe p);\nstatic void s1(char *p) { free(p); }\nvoid g(char *p) { s1(p); }\n"
    _write(tmp_path, "s.c", src)
    outs = [
        sorted(_tcc(tmp_path, o, "-fdiagnostics-parseable-fixits", "-c", "s.c", "-o", "s.o").stderr.splitlines())
        for o in ("-O0", "-O2")
    ]
    assert outs[0] == outs[1]
    assert ("s.c:2: error: the parameter 'p' owns nothing (a view unless it says owned): write '* owned p' when "
            "the function takes it over, 'drop' before the type too when it frees it") in outs[0]


def test_default_view_param_static_inline(tmp_path):
    src = "static inline int si(char *p) { return p[0]; }\nint g(char *p) { return si(p) + si(p); }\n"
    _write(tmp_path, "t.c", src)
    for o in ("-O0", "-O2"):
        proc = _tcc(tmp_path, o, "-c", "t.c", "-o", "t.o")
        assert proc.returncode == 0 and proc.stderr == "", proc.stderr


def test_default_view_param_of_a_function_pointer(tmp_path):
    # a parameter of a function pointer parameter is a view too
    src = "int f(int (*cb)(char *x), char *s) { return cb(s); }\n"
    _write(tmp_path, "t.c", src)
    proc = _tcc(tmp_path, "-c", "t.c", "-o", "t.o")
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr
    proc = _tcc(tmp_path, "-ccompat", "-c", "t.c", "-o", "t.o")
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


# ------------------------------------------------- locals, aliases, two on a line

LOCAL_SRC = """\
void * owned maybe malloc(unsigned long n);
void free(drop void * owned maybe p);
void take(char * owned p);
char * view maybe find(char *s);
int sum(char *s)
{
  char *p = s;
  return p[0];
}
void freeit(void)
{
  char *p = malloc(4);
  free(p);
}
char * owned maybe make(void)
{
  char *p = malloc(8);
  return p;
}
int found(char *s)
{
  char *hit = find(s);
  return hit ? hit[0] : 0;
}
int tested(char *s)
{
  char *p = s;
  if (p == nullptr)
    return 0;
  return p[1];
}
void hand(char * owned s)
{
  char *p = s;
  take(p);
}
void alias(char * owned s)
{
  char *q;
  q = s;
  free(q);
}
int alias_maybe(char *s)
{
  char *q = s;
  if (!q)
    return 0;
  return q[0];
}
int two(char *a, char *b) { return a[0] + b[0]; }
int twoloc(inout char *s) { char *a = s, *b = s + 1; return a[0] + b[0]; }
"""

LOCAL_NOTES = [
    # (line of, name, note, the line once fixed)
    # given an `in` parameter's value (a const target): const * view, not * view
    ("  char *p = s;\n", 7, "p", "the function only borrows through 'p': write 'const * view'",
     "  char const * view p = s;"),
    ("  char *p = malloc(4);", 12, "p",
     "the function gives 'p' an owned value that may be null (line 12): write '* owned maybe'",
     "  char * owned maybe p = malloc(4);"),
    ("  char *p = malloc(8);", 17, "p",
     "the function gives 'p' an owned value that may be null (line 17): write '* owned maybe'",
     "  char * owned maybe p = malloc(8);"),
    ("  char *hit", 22, "hit",
     "the function only borrows through 'hit' and gives it null or a maybe value (line 22): write '* view maybe'",
     "  char * view maybe hit = find(s);"),
    ("  char *p = s;\n  if", 27, "p",
     "the function only borrows through 'p' and tests it for null (line 28): write 'const * view maybe'",
     "  char const * view maybe p = s;"),
    ("void hand", 34, "p", "the function gives 'p' an owned value (line 34): write '* owned'", "  char * owned p = s;"),
    ("  char *q;", 39, "q", "the function gives 'q' an owned value (line 40): write '* owned'", "  char * owned q;"),
    ("  char *q = s;\n  if (!q)", 45, "q",
     "the function only borrows through 'q' and tests it for null (line 46): write 'const * view maybe'",
     "  char const * view maybe q = s;"),
]


def test_plain_local_and_alias_decided(tmp_path):
    proc, diags = _json(tmp_path, LOCAL_SRC)
    errs = [d for d in diags if d["kind"] == "error" and d["message"].startswith("plain pointer in yasc")]
    by = {(d["locations"][0]["caret"]["line"], d["message"]): d for d in errs}
    for _, ln, name, note, written in LOCAL_NOTES:
        d = by[(ln, f"plain pointer in yasc: say owned or view for '{name}'")]
        assert d["children"][0]["message"] == note, (ln, d)
        assert len(d["fixits"]) == 1, d
        assert _apply(LOCAL_SRC, d["fixits"]).splitlines()[ln - 1] == written, (ln, d["fixits"])
    fixed = _apply(LOCAL_SRC, _all_fixits(errs))
    # two locals on one line: each said, with its own fix-it (two parameters
    # are views without one)
    assert "int two(char *a, char *b) {" in fixed
    assert "{ char * view a = s, * view b = s + 1;" in fixed
    proc, diags = _json(tmp_path, fixed, "-Wall", name="fixed.c")
    assert proc.returncode != 0, proc.stderr
    assert {d.get("option") for d in diags} == {"-Wimplicit-move"}, diags
    fixed = _apply(fixed, _all_fixits(diags))
    _write(tmp_path, "fixed2.c", fixed)
    proc = _tcc(tmp_path, "-Wall", "-c", "fixed2.c", "-o", "f.o")
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


def test_plain_two_on_one_line_text(tmp_path):
    # the same place, two pointers: both errors and both notes are said
    src = "int g;\nvoid f(void) { int *a = &g, *b = &g; (void)a; (void)b; }\n"
    _write(tmp_path, "t.c", src)
    proc = _tcc(tmp_path, "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o", "t.o")
    assert proc.stderr.splitlines() == [
        "t.c:2: error: plain pointer in yasc: say owned or view for 'a'",
        "t.c:2: note: the function only borrows through 'a': write '* view'",
        'fix-it:"t.c":{2:20-2:22}:"* view a"',
        "t.c:2: error: plain pointer in yasc: say owned or view for 'b'",
        "t.c:2: note: the function only borrows through 'b': write '* view'",
        'fix-it:"t.c":{2:29-2:31}:"* view b"',
    ], proc.stderr


def test_plain_alias_given_something_else(tmp_path):
    # q copies s, then o: freeing q says nothing of s
    src = """\
void free(drop void * owned maybe p);
void f(char *s, char * owned o)
{
  char *q = s;
  int c = q[0];
  q = o;
  free(q);
  (void)c;
}
"""
    _write(tmp_path, "t.c", src)
    proc = _tcc(tmp_path, "-c", "t.c", "-o", "t.o")
    assert "for 's'" not in proc.stderr, proc.stderr  # a view parameter
    assert "t.c:4: note: the function gives 'q' an owned value (line 6): write '* owned'" in proc.stderr


def test_plain_local_not_decided(tmp_path):
    # an inner level, a static, a typedef: said at once, no note
    src = """\
void f(inout char * view *v)
{
  char **pp = v;
  static char *keep;
  typedef char *str;
  str t = nullptr;
  (void)pp;
  (void)keep;
  (void)t;
}
"""
    _write(tmp_path, "t.c", src)
    proc = _tcc(tmp_path, "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o", "t.o")
    assert proc.stderr.splitlines() == [
        "t.c:3: error: plain pointer in yasc: say owned or view for 'char *'",
        "t.c:4: error: plain pointer in yasc: say owned or view for 'char *'",
        "t.c:5: error: plain pointer in yasc: say owned or view for 'char *'",
        "t.c:3: error: plain pointer in yasc: say owned or view for 'pp'",
        "t.c:3: note: the function only borrows through 'pp': write '* view'",
        'fix-it:"t.c":{3:9-3:12}:"* view pp"',
    ]
    # -ccompat: no yasc, nothing
    proc = _tcc(tmp_path, "-ccompat", "-c", "t.c", "-o", "t.o")
    assert "plain pointer" not in proc.stderr


def test_plain_local_same_at_every_level(tmp_path):
    _write(tmp_path, "t.c", LOCAL_SRC)
    # a function's reports may come at the end of the unit at -O1+ (as its
    # other diagnostics do): the same reports
    outs = [
        sorted(_tcc(tmp_path, o, "-fmax-errors=0", "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o",
                    "t.o").stderr.splitlines())
        for o in ("-O0", "-O1", "-O2", "-Os", "-fsyntax-only")
    ]
    assert all(o == outs[0] for o in outs), outs
    src = "static inline int si(char *p) { char *q = p; return q[0]; }\nint g(char *p) { return si(p) + si(p); }\n"
    _write(tmp_path, "s.c", src)
    for o in ("-O0", "-O2"):
        proc = _tcc(tmp_path, o, "-c", "s.c", "-o", "s.o")
        assert proc.stderr.count("plain pointer") == 1, proc.stderr
        assert "s.c:1: note: the function only borrows through 'q': write 'const * view'" in proc.stderr


def test_plain_local_const_target_in_a_list(tmp_path):
    """given an `in` parameter's value, a local is `char const * view`: the
    const goes before its '*', which only the declaration's first declarator
    can take (`char *a = s, const * view b` is no C) -- the second one is
    said without a fix-it"""
    src = "#pragma yasc\nint f(char *s) { char *a = s, *b = s + 1; return a[0] + b[0]; }\n"
    _write(tmp_path, "t.c", src)
    proc = _tcc(tmp_path, "-fmax-errors=0", "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o", "t.o")
    lines = proc.stderr.splitlines()
    assert "t.c:2: note: the function only borrows through 'a': write 'const * view'" in lines, lines
    assert "t.c:2: note: the function only borrows through 'b': write 'const * view'" in lines, lines
    fixits = [line for line in lines if line.startswith("fix-it:")]
    assert fixits == ['fix-it:"t.c":{2:23-2:25}:"const * view a"'], lines
