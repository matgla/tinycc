"""yasc: a global's or a static's own pointer is a view unless it says owned.

Like a member's (test_yasc_member_view_default.py): `char *tzname[2] = {...}` at
file scope is `char *view tzname[2]` -- never null (`*?` says it may be),
never freed through it, never pointed at what dies before it (a local's
address, an owned value nobody frees).  Its inner levels are views too
(`char **g`), and so are an array's elements: a global array of pointers is
the common case (tables of names).  A local `static` and an `extern`
declaration follow the same rule.  `view` written anyway is -Wredundant-view.

Still said: a function's result (`char *f(void);`), a function pointer's
result (`char *(*cb)(int)`), a typedef's pointer.  A function pointer itself
stays exempt.

Never null is checked where a value is stored: a null constant in the
initializer (init/alloc.c) and null, or a pointer that may be null, stored by
a function (the lifetime check, lifetime.c).  A global left without an
initializer is zero, as a member left out of braces is: not said.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O2"]
PLAIN = "plain pointer in yasc: say owned or view"
REDUNDANT_GLOBAL = "a global's pointer is a view unless it says owned: drop 'view'"
REDUNDANT_STATIC = "a static's pointer is a view unless it says owned: drop 'view'"

PRELUDE = """\
#pragma yasc
void free(drop void *?owned p);
void *?owned malloc(unsigned long n);
inout char *?borrows(static) find(void);
inout char *borrows(static) get(void);
static char e0[1];
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _run(tmp_path, source, *flags, prelude=PRELUDE):
    src = tmp_path / "t.c"
    src.write_text(prelude + source)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-unused-variable", "-c", *flags, str(src), "-o",
         str(tmp_path / "t.o")],
        capture_output=True, text=True, cwd=tmp_path,
    )


def _diags(proc, prelude_lines=PRELUDE_LINES):
    """(line in the body, kind, message) of every diagnostic"""
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[1].isdigit() and parts[2].strip() in ("error", "warning", "note"):
            out.append((int(parts[1]) - prelude_lines, parts[2].strip(), parts[3].strip()))
    return out


def _errors(proc):
    return [m for _, k, m in _diags(proc) if k == "error"]


CLEAN = {
    "array_of_pointers": "static char utc[] = \"UTC\";\nchar *tzname[2] = {utc, utc};\n"
                         "int f(void) { return tzname[1][0]; }\n",
    "own_pointer": "static char buf[4] = \"abc\";\nchar *g = buf;\nint f(void) { return g[0]; }\n",
    "const_table": "static const char *const names[] = {\"a\", \"b\"};\nint f(int i) { return names[i][0]; }\n",
    "inner_levels": "static const char *list[] = {\"a\"};\nconst char **g = list;\nint f(void) { return g[0][0]; }\n",
    "several": "static char a[2], b[2];\nchar *p = a, *q = b;\nint f(void) { return p[0] + q[0]; }\n",
    "static_global": "static const char *g = \"x\";\nint f(void) { return g[0]; }\n",
    "extern": "extern char *e;\nint f(void) { return e[0]; }\n",
    "extern_then_definition": "extern char *e;\nstatic char b[2];\nchar *e = b;\n",
    "local_static": "int f(void) { static const char *s = \"x\"; return s[0]; }\n",
    "local_static_array": "int f(int i) { static const char *t[] = {\"a\", \"b\"}; return t[i][0]; }\n",
    "local_extern": "int f(void) { extern char *e; return e[0]; }\n",
    "maybe": "char *?m;\nvoid f(void) { m = nullptr; m = find(); }\n",
    "maybe_array": "const char *?t[2] = {\"a\", nullptr};\n",
    "owned": "char *?owned o;\nvoid f(void) { free(o); o = malloc(4); }\n",
    "function_pointer": "void (*handler)(int);\nvoid f(void) { handler(1); }\n",
    "function_pointer_param": "int (*cb)(char *p);\n",
    "store_static": "static char buf[4];\nchar *g = buf;\nvoid f(void) { g = get(); }\n",
    "store_tested": "char *g = e0;\nvoid f(void) { char *?m = find(); if (m) g = m; }\n",
    "struct_global": "struct s { char *p; };\nstatic char b[2];\nstruct s g = {.p = b};\n",
    "left_out": "char *g;\nint f(void) { return g[0]; }\n",
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(CLEAN))
def test_clean(tmp_path, name, opt):
    proc = _run(tmp_path, CLEAN[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


VIEW_RULES = {
    # a view is never freed
    "freed": ("static char b[2];\nchar *g = b;\nvoid f(void) { free(g); }\n",
              "a pointer that owns nothing is stored in an owned pointer"),
    # nor given what nobody frees
    "owned_value": ("char *g = e0;\nvoid f(void) { g = malloc(4); }\n",
                    "an owned result is stored in a pointer that owns nothing: it is never freed"),
    # nor pointed at what dies before it
    "local_address": ("char *g = e0;\nvoid f(void) { char buf[4] = {0}; g = buf; }\n",
                      "the address of a local variable is stored in global 'g', which outlives 'f'"),
    "local_static_local_address": ("void f(void) { static char *s = e0; char buf[4] = {0}; s = buf; }\n",
                                   "the address of a local variable is stored in global 's', which outlives 'f'"),
}


@pytest.mark.parametrize("name", sorted(VIEW_RULES))
def test_plain_global_is_a_view(tmp_path, name):
    source, message = VIEW_RULES[name]
    proc = _run(tmp_path, source)
    assert proc.returncode != 0 and message in _errors(proc), proc.stderr
    written = _run(tmp_path, source.replace("char *", "char *view "), "-Wno-redundant-view")
    assert _diags(written) == _diags(proc), written.stderr


def test_written_view_is_redundant(tmp_path):
    proc = _run(tmp_path, "static char b[2];\nchar *view p = b;\nchar *?view q;\n"
                          "char *view *view r;\nchar *view t[1] = {b};\n"
                          "int f(void) {\n  static char *view s = e0;\n  return s[0];\n}\n", "-Wredundant-view")
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [(2, "warning", REDUNDANT_GLOBAL), (3, "warning", REDUNDANT_GLOBAL),
                            (4, "warning", REDUNDANT_GLOBAL), (5, "warning", REDUNDANT_GLOBAL),
                            (7, "warning", REDUNDANT_STATIC)], proc.stderr


NOT_REDUNDANT = {
    "inner_level_only": "char *view *?owned p;\n",
    "owned": "char *?owned p;\n",
    "function_pointer": "int (*view cb)(int);\n",
    "borrows_static": "char *?borrows(static) p;\n",
}


@pytest.mark.parametrize("name", sorted(NOT_REDUNDANT))
def test_not_redundant(tmp_path, name):
    proc = _run(tmp_path, NOT_REDUNDANT[name], "-Wredundant-view")
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


PLAIN_STILL = {
    "function_result": "char *f(void);\n",
    "function_pointer_result": "char *(*get_cb)(void);\n",
    "typedef": "typedef char *str;\n",
    "local_function_result": "void g(void) { char *h(void); }\n",
}


@pytest.mark.parametrize("name", sorted(PLAIN_STILL))
def test_other_pointers_still_say_it(tmp_path, name):
    proc = _run(tmp_path, PLAIN_STILL[name])
    # a function's result is named by the function (`f`)
    what = "the result of 'f' ('char *')" if name == "function_result" else "'char *'"
    assert proc.returncode != 0 and PLAIN + " for " + what in _errors(proc), proc.stderr


def test_regular_c_global_is_unchecked(tmp_path):
    """a global in a c_begin section is regular C: no word needed, and yasc code
    that reads through it says unsafe"""
    proc = _run(tmp_path, "#pragma tcc c_begin\nextern char *e;\n#pragma tcc c_end\n"
                          "int f(void) { return e[0]; }\n")
    assert _errors(proc) == ["a pointer from regular C is dereferenced: write unsafe { } around it"], proc.stderr


STORED_NULL = "a null pointer is stored in '{}', which is not '?'"
STORED_MAYBE = "a pointer that may be null is stored in '{}', which is not '?'"

NULL_ERRORS = {
    "nullptr": ("char *g = e0;\nvoid f(void) { g = nullptr; }\n", STORED_NULL.format("g")),
    "maybe_result": ("char *g = e0;\nvoid f(void) { g = find(); }\n", STORED_MAYBE.format("g")),
    "array_element": ("char *t[2] = {e0, e0};\nvoid f(void) { t[1] = nullptr; }\n", STORED_NULL.format("t")),
    "static_global": ("static char *g = e0;\nvoid f(void) { g = nullptr; }\nint r(void) { return g[0]; }\n",
                      STORED_NULL.format("g")),
    "local_static": ("int f(void) { static char *s = e0; s = nullptr; return s[0]; }\n", STORED_NULL.format("s")),
    "owned": ("char *owned o;\nvoid f(void) { free(o); o = nullptr; }\n", STORED_NULL.format("o")),
    "init": ("char *g = nullptr;\n", "a null pointer initializes a pointer that is not '?'"),
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(NULL_ERRORS))
def test_null_into_a_global_that_is_not_maybe(tmp_path, name, opt):
    source, message = NULL_ERRORS[name]
    proc = _run(tmp_path, source, opt)
    assert proc.returncode != 0 and _errors(proc) == [message], proc.stderr
