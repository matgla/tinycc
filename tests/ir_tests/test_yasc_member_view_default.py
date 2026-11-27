"""yasc: a struct or union member's own pointer is a view unless it says owned.

Like a parameter's (test_yasc_param_view_default.py): `struct s { const char *name; }`
is `const char *view name` -- never null (`*?` says it may be), never
freed through the member, never pointed at what dies before the struct.  Its
inner levels are views too (`const char *const *list`).  Only what changes
hands is written (`owned`, `borrows(...)`), with `maybe`.  `view` written on a
member's own pointer anyway is -Wredundant-view.  An array's element pointers
are views too (test_yasc_nested_view_default.py).  A function pointer's
result and a slice's element still say it; a function
pointer stays exempt, and its parameters are parameters.

The C header written from a yasc one (-yasc-c-header) marks a plain member
pointer __yl_view, as it does a written `view` one, so a C unit's yasc
callers believe it the same way.

Never null is checked where a value is stored: null (or a pointer that may be
null) stored through `->` / `.` (the lifetime check, after the tests that
refine it), and a null constant in an initializer (a member, an array
element, a static pointer).  A compound literal's plain pointers are views
too (`(const char *?[]){"a", nullptr}`); sizeof's and a C cast's
still say it.

`c_api(T)` is a yasc word: in regular C -- a `#pragma tcc c_begin` section, a
file without `#pragma yasc` -- it is an error that says so, not C's reading of
it as a function named c_api ("invalid type for 'c_api'").
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O2"]
PLAIN = "plain pointer in yasc: say owned or view"
REDUNDANT = "a member's pointer is a view unless it says owned: drop 'view'"
CAPI_C_SECTION = ("'c_api(...)' is a yasc word, and this is regular C (a '#pragma tcc c_begin' section): "
                  "put the declaration after '#pragma tcc c_end'")
CAPI_C_FILE = ("'c_api(...)' is a yasc word, and this is regular C (this file does not say '#pragma yasc'): "
               "a C declaration writes the C type alone")

PRELUDE = """\
#pragma yasc
void free(drop void *?owned p);
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _run(tmp_path, source, *flags, prelude=PRELUDE):
    src = tmp_path / "t.c"
    src.write_text(prelude + source)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
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


CLEAN = {
    "own_pointer": "struct s { const char *name; };\nint f(struct s *x) { return x->name[0]; }\n",
    "inner_levels": "struct s { const char *const *list; };\nint f(struct s *x) { return x->list[0][0]; }\n",
    "array_of_pointers": "struct s { const char *a[2]; };\nint f(struct s *x) { return x->a[1][0]; }\n",
    "several": "struct s { char *a, *b; int n; };\nint f(struct s *x) { return x->a[0] + x->b[0] + x->n; }\n",
    "nested": "struct o { struct { char *p; } in; union { char *u; int i; } un; };\n"
              "int f(struct o *x) { return x->in.p[0]; }\n",
    "typedef_struct": "typedef struct s { char *p; } s;\nint f(s *x) { return x->p[0]; }\n",
    "maybe": "struct s { char *?p; };\nvoid f(inout struct s *x) { x->p = nullptr; }\n",
    "owned": "struct s { char *owned p; };\nvoid f(drop struct s *owned x) { free(x->p); free(x); }\n",
    "function_pointer": "struct s { int (*cb)(int); };\nint f(struct s *x) { return x->cb(1); }\n",
    "function_pointer_param": "struct s { int (*cb)(char *p); char *arg; };\nint f(struct s *x) { return x->cb(x->arg); }\n",
    "c_api": "struct s { const char *c_api(char *) name; };\nint f(struct s *x) { return x->name[0]; }\n",
    "tagged_union": "enum vk { V_INT, V_STR };\nstruct v { enum vk k; union(k) { long i when(V_INT); char *s when(V_STR); } u; };\n"
                    "int f(struct v *x) { return x->k == V_STR ? x->u.s[0] : 0; }\n",
    "bitfield_neighbour": "struct s { char *p; unsigned f : 3; };\nint f(struct s *x) { return x->p[0] + x->f; }\n",
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(CLEAN))
def test_clean(tmp_path, name, opt):
    proc = _run(tmp_path, CLEAN[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


VIEW_RULES = {
    # a view is never freed: as a written `view` member
    "freed": ("struct s { char *p; };\nvoid f(inout struct s *x) { free(x->p); }\n",
              "a pointer that owns nothing is stored in an owned pointer"),
    # nor pointed at what dies before it
    "local_address": ("struct s { char *p; };\nvoid f(inout struct s *x) { char buf[4] = {0}; x->p = buf; }\n",
                      "the address of a local variable is stored through 'x', which outlives 'f'"),
}


@pytest.mark.parametrize("name", sorted(VIEW_RULES))
def test_plain_member_is_a_view(tmp_path, name):
    source, message = VIEW_RULES[name]
    proc = _run(tmp_path, source)
    assert proc.returncode != 0 and (2, "error", message) in _diags(proc), proc.stderr
    written = _run(tmp_path, source.replace("char *p;", "char *view p;"), "-Wno-redundant-view")
    assert _diags(written) == _diags(proc), written.stderr


def test_written_view_is_redundant(tmp_path):
    proc = _run(tmp_path, "struct s {\n  char *view p;\n  char *?view q;\n  char *view *view r;\n};\n")
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [(2, "warning", REDUNDANT), (3, "warning", REDUNDANT), (4, "warning", REDUNDANT)], proc.stderr


NOT_REDUNDANT = {
    "inner_level_only": "struct s { char *view *owned p; };\n",
    "owned": "struct s { char *owned p; };\n",
    "function_pointer": "struct s { int (*view cb)(int); };\n",
    "parameter_of_a_member": "struct s { int (*cb)(char *owned p); };\n",
}


@pytest.mark.parametrize("name", sorted(NOT_REDUNDANT))
def test_not_redundant(tmp_path, name):
    proc = _run(tmp_path, NOT_REDUNDANT[name])
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


PLAIN_STILL = {
    "function_pointer_result": "struct s { char *(*get)(void); };\n",
    "typedef": "typedef char *str;\n",
}


@pytest.mark.parametrize("name", sorted(PLAIN_STILL))
def test_other_pointers_still_say_it(tmp_path, name):
    proc = _run(tmp_path, PLAIN_STILL[name])
    assert proc.returncode != 0 and (1, "error", PLAIN + " for 'char *'") in _diags(proc), proc.stderr


def test_regular_c_member_is_unchecked(tmp_path):
    """a member in a c_begin section is regular C, not a view: no word needed, and
    yasc code that reads through it says unsafe"""
    proc = _run(tmp_path, "#pragma tcc c_begin\nstruct s { char *p; };\n#pragma tcc c_end\n"
                          "int f(struct s *x) { return x->p[0]; }\n")
    assert _diags(proc) == [(4, "error", "a pointer from regular C is dereferenced: write unsafe { } around it")], \
        proc.stderr


def test_c_api_in_a_c_section(tmp_path):
    proc = _run(tmp_path, "#pragma tcc c_begin\nstruct s { const char *c_api(char *) name; int n; };\n"
                          "#pragma tcc c_end\nint f(struct s *x) { return x->n; }\n")
    # one error, and the member is still read: no "invalid type", no "field not found"
    assert _diags(proc) == [(2, "error", CAPI_C_SECTION)], proc.stderr


def test_c_api_in_a_c_file(tmp_path):
    proc = _run(tmp_path, "struct s { const char *c_api(char *) name; };\nconst char *c_api(char *) get(void);\n",
                "-ccompat", prelude="")
    assert _diags(proc, 0) == [(1, "error", CAPI_C_FILE), (2, "error", CAPI_C_FILE)], proc.stderr


def test_c_api_as_a_name_is_c(tmp_path):
    """a C function that happens to be called c_api is not the yasc word"""
    proc = _run(tmp_path, "int c_api;\nint get(void) { return c_api; }\n", "-ccompat", prelude="")
    assert proc.returncode == 0, proc.stderr


def _c_header(tmp_path, source):
    inc = tmp_path / "inc"
    (inc / "yasc").mkdir(parents=True)
    (inc / "yasc" / "m.h").write_text(source)
    return subprocess.run([str(TCC), "-yasc-c-header", "yasc/m.h"], cwd=inc, capture_output=True, text=True)


def test_c_header_marks_plain_members(tmp_path):
    proc = _c_header(tmp_path, "#pragma yasc\nstruct s {\n  const char *c_api(char *) name;\n  char *p;\n"
                               "  char *?q;\n  char *owned o;\n  int (*cb)(char *a);\n  char *arr[2];\n  int n;\n};\n")
    assert proc.returncode == 0, proc.stderr
    body = proc.stdout[proc.stdout.index("struct s {"):]
    body = body[:body.index("};") + 2]
    assert body == ("struct s {\n  char *name __yl_view;\n  char *p __yl_view;\n  char *q __yl_view_maybe;\n"
                    "  char *o;\n  int (*cb)(const char *a);\n  char *arr[2];\n  int n;\n};"), proc.stdout


NULL_PRELUDE = PRELUDE + "inout char *?borrows(static) find(void);\ninout char *borrows(static) get(void);\n"
NULL_PRELUDE_LINES = NULL_PRELUDE.count("\n")
STORED_NULL = "a null pointer is stored in '{}', which is not '?'"
STORED_MAYBE = "a pointer that may be null is stored in '{}', which is not '?'"
INIT_NULL = "a null pointer initializes '{}', which is not '?'"
INIT_ELEMENT = "a null pointer initializes an element that is not '?'"
INIT_MAYBE = "a pointer that may be null initializes '{}', which is not '?'"
INIT_MAYBE_ELEMENT = "a pointer that may be null initializes an element that is not '?'"

NULL_ERRORS = {
    "nullptr": ("struct s { char *p; };\nvoid f(inout struct s *x) { x->p = nullptr; }\n", STORED_NULL.format("x->p")),
    "maybe_result": ("struct s { char *p; };\nvoid f(inout struct s *x) { x->p = find(); }\n", STORED_MAYBE.format("x->p")),
    "maybe_member": ("struct s { char *p; char *?m; };\nvoid f(inout struct s *x) { x->p = x->m; }\n",
                     STORED_MAYBE.format("x->p")),
    "owned": ("struct s { char *owned p; };\nvoid f(inout struct s *x) { free(x->p); x->p = nullptr; }\n",
              STORED_NULL.format("x->p")),
    "local_struct": ("struct s { char *p; int n; };\nint f(void) { char b[2] = {0}; struct s y = {.p = b, .n = 1}; "
                     "y.p = nullptr; return y.n; }\n", STORED_NULL.format("y.p")),
    "init_local": ("struct s { char *p; int n; };\nint f(void) { struct s y = {.p = nullptr, .n = 1}; return y.n; }\n",
                   INIT_NULL.format("p")),
    "init_zero": ("struct s { char *p; int n; };\nint f(void) { struct s y = {nullptr, 1}; return y.n; }\n",
                  INIT_NULL.format("p")),
    "init_global": ("struct s { char *p; };\nstruct s g = {.p = nullptr};\n", INIT_NULL.format("p")),
    "init_maybe_value": ("struct s { char *p; int n; };\nint f(void) { struct s y = {.p = find(), .n = 1}; return y.n; }\n",
                         INIT_MAYBE.format("p")),
    "init_maybe_positional": ("struct s { char *p; int n; };\n"
                              "int f(void) { char *?m = find(); struct s y = {m, 1}; return y.n; }\n",
                              INIT_MAYBE.format("p")),
    "init_maybe_element": ("struct g { char *const *mem; };\n"
                           "int f(void) { struct g x = {.mem = (char *[]){get(), find()}}; return x.mem[0][0]; }\n",
                           INIT_MAYBE_ELEMENT),
    "global_nullptr": ("struct s { int n; char *p; };\nstruct s g;\nvoid f(void) { g.p = nullptr; }\n",
                       STORED_NULL.format("g.p")),
    "global_maybe": ("struct s { int n; char *p; };\nstruct s g;\nvoid f(void) { g.p = find(); }\n",
                     STORED_MAYBE.format("g.p")),
    "init_element": ("struct g { const char *const *mem; };\n"
                     "struct g x = {.mem = (const char *[]){\"a\", nullptr}};\n", INIT_ELEMENT),
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(NULL_ERRORS))
def test_null_into_a_member_that_is_not_maybe(tmp_path, name, opt):
    source, message = NULL_ERRORS[name]
    proc = _run(tmp_path, source, opt, prelude=NULL_PRELUDE)
    d = _diags(proc, NULL_PRELUDE_LINES)
    assert proc.returncode != 0 and [m for _, k, m in d if k == "error"] == [message], proc.stderr


NULL_FINE = {
    "tested": "struct s { char *p; };\nvoid f(inout struct s *x) { char *?m = find(); if (m) x->p = m; }\n",
    "tested_member": "struct s { char *p; char *?m; };\nvoid f(inout struct s *x) { if (x->m) x->p = x->m; }\n",
    "never_null": "struct s { char *p; };\nvoid f(inout struct s *x) { x->p = get(); }\n",
    "maybe_member": "struct s { char *?m; };\nvoid f(inout struct s *x) { x->m = nullptr; x->m = find(); }\n",
    "init_maybe": "struct s { char *?m; int n; };\nint f(void) { struct s y = {.m = nullptr, .n = 1}; return y.n; }\n",
    "init_value": "struct s { char *p; int n; };\nint f(void) { struct s y = {.p = get(), .n = 1}; return y.n; }\n",
    "init_tested": "struct s { char *p; int n; };\n"
                   "int f(void) { char *?m = find(); if (!m) return 0; struct s y = {.p = m, .n = 1}; return y.n; }\n",
    "global_value": "struct s { int n; char *p; };\nstruct s g;\nvoid f(void) { g.p = get(); }\n",
    "global_tested": "struct s { int n; char *p; };\nstruct s g;\n"
                     "void f(void) { char *?m = find(); if (m) g.p = m; }\n",
    "left_out": "struct s { char *p; int n; };\nint f(void) { struct s y = {.n = 1}; return y.n; }\n",
    "maybe_elements": "struct g { const char *?const *mem; };\n"
                      "struct g x = {.mem = (const char *?[]){\"a\", nullptr}};\n",
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(NULL_FINE))
def test_what_may_go_in(tmp_path, name, opt):
    proc = _run(tmp_path, NULL_FINE[name], opt, prelude=NULL_PRELUDE)
    assert proc.returncode == 0 and _diags(proc, NULL_PRELUDE_LINES) == [], proc.stderr


COMPOUND = {
    "elements": "int f(void) { const char *@(view, const) *a = (const char *[]){\"a\", \"b\"}; return a[1][0]; }\n",
    "struct_pointer": "struct s { int n; };\nint f(void) { struct s *p = &(struct s){1}; return p->n; }\n",
    "member_of_literal": "struct s { const char *name; };\n"
                         "int f(void) { struct s *p = &(struct s){.name = \"n\"}; return p->name[0]; }\n",
}


@pytest.mark.parametrize("name", sorted(COMPOUND))
def test_compound_literal_pointers_are_views(tmp_path, name):
    proc = _run(tmp_path, COMPOUND[name])
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


@pytest.mark.parametrize("source", ["int f(void) { return sizeof(char *); }\n",
                                    "long f(long x) { return (long)(char *)x; }\n"], ids=["sizeof", "c_cast"])
def test_other_type_names_still_say_it(tmp_path, source):
    proc = _run(tmp_path, source)
    assert (1, "error", PLAIN + " for 'char *'") in _diags(proc), proc.stderr
