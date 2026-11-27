"""yasc: a function's view result is read-only on every level, as an in
parameter is, unless `inout` before the function's type says it may be
written through.  That is what the body returns into: `return s;` of an in
parameter needs no cast.

A result whose sources are parameters only (`borrows(s)`, or the elision) is
as writable as the arguments passed for them, level by level -- C23's
strchr: `strchr(buf, c)` of a char buf is a char *, of a const one a const
char *.  A result that borrows static memory is as declared.

A plain local takes a read-only value: what flows into it is followed, as
for a value that may be null.  Written through, handed to a parameter that
may write through it, stored where it may be written through, or returned as
a writable result, it is an error; reading it is not.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O2"]

PRELUDE = """\
#pragma yasc
struct grp { int n; char *name; };
struct grp *?borrows(static) keep(int k);
char *borrows(static) err(int e);
inout char *borrows(static) werr(int e);
char *?borrows(s) chr(char *s, int c);
void *borrows(base) bs(void *key, void *base);
void wr(inout char *p);
void rd(char *p);
struct holder { char *mut; const char *ro; };
"""
PRELUDE_LINES = PRELUDE.count("\n")

WRITE = "writing through '{}', which may hold a read-only value (a const one was stored in it)"


def _run(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True, text=True, cwd=tmp_path,
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[1], parts[2]))
    return out


CLEAN = {
    "return_in_parameter": "char *borrows(s) f(char *s) { return s + 1; }\n",
    "writable_argument": "int f(inout char *buf) { char *?p = chr(buf, ':'); if (p) *p = 0; return 0; }\n",
    "void_result": "int f(inout int *arr) { int *p = bs(arr, arr); *p = 1; return 0; }\n",
    "const_argument": "int f(void) { const char *k = \"x\"; const char *?p = chr(k, 'x'); return p ? *p : 0; }\n",
    "inout_static": "int f(void) { char *t = werr(1); t[0] = 0; return 0; }\n",
    "local_reads": "int f(void) { char *e = err(1); rd(e); return e[0]; }\n",
    "flows_to_a_const_result": "struct grp *?borrows(static) f(int k)\n{\n  struct grp *?found = nullptr;\n"
                               "  if (k)\n    found = keep(k);\n  return found;\n}\n",
    "reassigned": "int f(inout char *buf) { char *e = err(1); e = buf; e[0] = 0; return 0; }\n",
    "const_member": "void f(inout struct holder *h) { char *e = err(1); h->ro = e; }\n",
    "inout_back": "char *borrows(static) f(void) { char *e = werr(1); return e; }\n",
    "nested_inline": "static inline char *view next(inout char *s) { return s + 1; }\n"
                     "void f(inout char *b) { char *p = next(next(b)); *p = 0; }\n",
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(CLEAN))
def test_clean(tmp_path, name, opt):
    proc = _run(tmp_path, CLEAN[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


REPORTED = {
    "write_static": ("int f(void) { char *e = err(1); e[0] = 0; return 0; }\n", [(1, WRITE.format("e"))]),
    "const_argument": ("int f(char *ro) { char *?p = chr(ro, ':'); if (p) *p = 0; return 0; }\n",
                       [(1, WRITE.format("p"))]),
    "member_of_static": ("int f(void) { struct grp *?g = keep(1); if (g) g->n = 1; return 0; }\n",
                         [(1, WRITE.format("g"))]),
    "copy": ("int f(void) { char *e = err(1); char *c = e + 1; c[0] = 0; return 0; }\n", [(1, WRITE.format("c"))]),
    "through_a_call": ("int f(void) { char *e = err(1); char *?c = chr(e, 'x'); if (c) *c = 0; return 0; }\n",
                       [(1, WRITE.format("c"))]),
    "to_a_writing_parameter": ("int f(void) { char *e = err(1); wr(e); return 0; }\n",
                               [(1, "'e' may hold a read-only value: parameter 1 of 'wr' may write through it")]),
    "to_a_writable_member": ("void f(inout struct holder *h) { char *e = err(1); h->mut = e; }\n",
                             [(1, "'e' may hold a read-only value: stored where it may be written through")]),
    "as_a_writable_result": ("inout char *borrows(static) f(void) { char *e = err(1); return e; }\n",
                             [(1, "'e' may hold a read-only value: the function's result may be written through")]),
    "inner_level_of_an_in_parameter": ("int f(char **pp) { char *p = pp[0]; p[0] = 'x'; return 0; }\n",
                                       [(1, WRITE.format("p"))]),
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(REPORTED))
def test_reported(tmp_path, name, opt):
    body, want = REPORTED[name]
    proc = _run(tmp_path, body, opt)
    assert proc.returncode != 0
    assert [(l, m) for l, k, m in _diags(proc) if k == "error"] == want, proc.stderr


TYPE_ERRORS = {
    # not a plain local: the type says it
    "static_into_a_global": "char *g = nullptr;\nvoid f(void) { g = err(1); }\n",
    "static_into_owned": "void free(drop void *?owned p);\nvoid f(void) { char *owned o = err(1); free(o); }\n",
    "inout_result_from_in": "inout char *view f(char *s) { return s; }\n",
}


@pytest.mark.parametrize("name", sorted(TYPE_ERRORS))
def test_discards_where_no_local_takes_it(tmp_path, name):
    proc = _run(tmp_path, TYPE_ERRORS[name])
    assert proc.returncode != 0
    assert any("discards 'const'" in m for _, _, m in _diags(proc)), proc.stderr


def test_inout_only_before_a_function_with_a_view_result(tmp_path):
    for body in ("inout int f(void);\n", "void f(void) { inout char *p = err(1); (void)p; }\n",
                 "inout char *owned f(void);\n"):
        proc = _run(tmp_path, body)
        assert "inout before the type: only a function's view result is written through" in proc.stderr, (body, proc.stderr)


def test_declaration_and_definition_agree(tmp_path):
    proc = _run(tmp_path, "inout char *borrows(static) g(void);\nstatic char b[2];\n"
                          "char *borrows(static) g(void) { return b; }\n")
    assert proc.returncode != 0 and "the result differs in in / inout" in proc.stderr, proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_a_write_through_a_borrowing_result_keeps_inout(tmp_path, opt):
    # -Wparam-access: dropping inout would make the write an error
    proc = _run(tmp_path, "int f(inout char *buf) { char *?p = chr(buf, ':'); if (p) *p = 0; return 0; }\n", opt)
    assert "is inout, but" not in proc.stderr, proc.stderr


def _cheader(tmp_path, text):
    inc = tmp_path / "inc"
    (inc / "yasc").mkdir(parents=True, exist_ok=True)
    (inc / "sys").mkdir(exist_ok=True)
    (inc / "sys" / "_attrs.h").write_text("#pragma once\n#define __yl_c_api(n, ...)\n")
    (inc / "yasc" / "x.h").write_text(text)
    proc = subprocess.run([str(TCC), "-yasc-c-header", "yasc/x.h"], cwd=inc, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return proc.stdout


def test_c_header_drops_inout_and_says_the_yasc_result(tmp_path):
    out = _cheader(tmp_path, "#pragma yasc\n#include <sys/_attrs.h>\n"
                             "inout struct tm *?borrows(static) lt(long *t);\n"
                             "char *?@(borrows(static), c_api(char *)) err(int e);\n"
                             "inout char *?@(borrows(static), c_api(char *)) werr(int e);\n")
    assert "struct tm *lt(const long *t);" in out, out
    assert "char *err(int e) __yl_c_api(0, const char *);" in out, out
    assert "char *werr(int e) __yl_c_api(0, char *);" in out, out
    assert "inout" not in out, out


def test_module_header_drops_inout(tmp_path):
    src = tmp_path / "m.c"
    src.write_text("#pragma yasc\nmodule rtest;\nstatic char buf[4];\n"
                   "export inout char *borrows(static) f(void) { return buf; }\n")
    inc = tmp_path / "inc"
    proc = subprocess.run([str(TCC), "-ccompat", f"-fmodule-header={inc}", "-c", str(src), "-o", str(tmp_path / "m.o")],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    text = "".join(p.read_text() for p in inc.glob("*.h"))
    assert "char * f(void);" in text and "inout" not in text, text
