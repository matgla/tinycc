"""Strict lifetimes (S1) and `unsafe` (S2), docs/static_analysis_todo.md.

In the safe default (no -ccompat) -Wlifetime, -Wimplicit-move,
-Wdangling-pointer and -Wreturn-local-addr are errors, and so are the yasc
ownership rules the types alone break.  Neither -Wno-..., -Wno-error=... nor
#pragma diagnostic lowers them: only `unsafe { ... }` or `unsafe(expr)`,
which turn them off for the lines of the region -- for what the parse says
and for the flow checks, which report after the body, at every -O.
-ccompat keeps them warnings.  `unsafe` is a keyword only in yasc, and a
name of that spelling in scope wins.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

PRELUDE = """\
void * owned maybe malloc(unsigned n);
void free(drop void * owned maybe p);
void take(char * owned p);
int use(char * view p);
int * view g;
"""
PRELUDE_LINES = PRELUDE.count("\n")

TWICE = "void f(void)\n{\n  char * owned maybe p = malloc(1);\n  free(p);\n  free(p);\n}\n"
TWICE_MSG = "'p' is freed twice"


def _compile(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-unused-parameter", "-Wno-shadow", "-Wno-cross-tu", "-Wno-tainted-format", "-c", "-fmax-errors=0", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            where = parts[0].rsplit(":", 1)
            line = int(where[1]) - PRELUDE_LINES if len(where) == 2 and where[1].isdigit() else 0
            out.append((line, parts[1], parts[2]))
    return out


# ---- S1: errors that cannot be lowered ----


@pytest.mark.parametrize("opt", LEVELS)
def test_violation_is_an_error(tmp_path, opt):
    proc = _compile(tmp_path, TWICE, opt)
    assert proc.returncode != 0
    assert (5, "error", TWICE_MSG) in _diags(proc), proc.stderr


@pytest.mark.parametrize("flag", ["-Wno-lifetime", "-Wno-error=lifetime"])
def test_command_line_cannot_lower(tmp_path, flag):
    proc = _compile(tmp_path, TWICE, flag)
    assert proc.returncode != 0
    d = _diags(proc)
    assert (5, "error", TWICE_MSG) in d, proc.stderr
    assert any("cannot be lowered" in m for _, _, m in d), proc.stderr


RETLOCAL = "int * view f(int * view p)\n{\n  int x = *p;\n  return &x;\n}\n"
RETLOCAL_MSG = "'f' returns the address of a local variable"


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("group", ["-Wall", "-Wextra"])
def test_pragma_group_cannot_lower(tmp_path, group, opt):
    """-Wreturn-local-addr is in -Wall: ignoring the group leaves it an error."""
    body = f'#pragma GCC diagnostic ignored "{group}"\n' + RETLOCAL + TWICE.replace("void f(", "void h(")
    proc = _compile(tmp_path, body, opt)
    d = _diags(proc)
    assert proc.returncode != 0, proc.stderr
    assert any(l == 5 and k == "error" and RETLOCAL_MSG in m for l, k, m in d), proc.stderr
    assert (11, "error", TWICE_MSG) in d, proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_pragma_pop_after_unsafe_keeps_strict(tmp_path, opt):
    """A push inside unsafe saved the strict options off: a pop after the
    region does not leave them off."""
    body = (
        "void e(void)\n{\n  unsafe {\n#pragma GCC diagnostic push\n  }\n}\n"
        "#pragma GCC diagnostic pop\n" + TWICE
    )
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode != 0, proc.stderr
    assert (12, "error", TWICE_MSG) in _diags(proc), proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_pragma_pop_inside_unsafe_keeps_it_unsafe(tmp_path, opt):
    body = (
        "#pragma GCC diagnostic push\nvoid f(void)\n{\n  unsafe {\n#pragma GCC diagnostic pop\n"
        "    char * owned maybe p = malloc(1);\n    free(p);\n    free(p);\n  }\n}\n"
    )
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_pragma_cannot_lower(tmp_path):
    body = '#pragma GCC diagnostic ignored "-Wlifetime"\n' + TWICE
    proc = _compile(tmp_path, body)
    assert proc.returncode != 0
    d = _diags(proc)
    assert (6, "error", TWICE_MSG) in d, proc.stderr
    assert any("cannot be lowered" in m for _, _, m in d), proc.stderr


def test_ccompat_keeps_warnings(tmp_path):
    proc = _compile(tmp_path, TWICE, "-ccompat", "-std=yasc", "-Wlifetime")
    assert proc.returncode == 0, proc.stderr
    assert (5, "warning", TWICE_MSG) in _diags(proc), proc.stderr


def test_type_rule_is_an_error(tmp_path):
    body = "void f(inout char * view v)\n{\n  char * owned p = v;\n  take(p);\n}\n"
    proc = _compile(tmp_path, body)
    assert proc.returncode != 0
    assert (3, "error", "a pointer that owns nothing is stored in an owned pointer") in _diags(proc), proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_dangling_is_an_error(tmp_path, opt):
    body = "void f(void)\n{\n  int x = 1;\n  g = &x;\n}\n"
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode != 0
    assert any(k == "error" and "stored in global 'g'" in m for _, k, m in _diags(proc)), proc.stderr


# ---- S2: unsafe ----

UNSAFE_BLOCK = (
    "void f(void)\n{\n  char * owned maybe p = malloc(1);\n  free(p);\n  unsafe {\n    free(p);\n  }\n}\n"
)
UNSAFE_EXPR = "void f(void)\n{\n  char * owned maybe p = malloc(1);\n  free(p);\n  unsafe(free(p));\n}\n"


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("body", [UNSAFE_BLOCK, UNSAFE_EXPR], ids=["block", "expr"])
def test_unsafe_lets_it_pass(tmp_path, opt, body):
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_after_the_region_it_is_said_again(tmp_path, opt):
    """After the region the checks speak again, for another variable and for
    the one the region let pass (the region ends at the line of the first
    token after it)."""
    body = (
        "void f(void)\n{\n  char * owned maybe p = malloc(1);\n  char * owned maybe q = malloc(1);\n  free(p);\n"
        "  unsafe {\n    free(p);\n  }\n  free(q);\n  free(q);\n}\n"
    )
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode != 0
    assert [(l, k) for l, k, _ in _diags(proc)] == [(10, "error")], proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_nested_regions(tmp_path, opt):
    body = (
        "void f(void)\n{\n  char * owned maybe p = malloc(1);\n  char * owned maybe q = malloc(1);\n"
        "  char * owned maybe r = malloc(1);\n  free(p);\n  free(q);\n  unsafe {\n    unsafe {\n"
        "      free(p);\n    }\n    free(q);\n  }\n  free(r);\n  free(r);\n}\n"
    )
    proc = _compile(tmp_path, body, opt)
    assert [(l, k) for l, k, _ in _diags(proc)] == [(15, "error")], proc.stderr


def test_unsafe_covers_the_frontend_rules(tmp_path):
    """-Wimplicit-move and the ownership type rules, said while the body is
    parsed, are off in the region too."""
    body = (
        "void f(inout char * view v)\n{\n  char * owned maybe p = malloc(1);\n  unsafe {\n    take(p);\n"
        "    char * owned q = v;\n    take(q);\n  }\n}\n"
    )
    proc = _compile(tmp_path, body)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_unsafe_in_a_static_inline(tmp_path, opt):
    """A body compiled at the end of the unit (-O1+) still sees its region,
    and a violation after it in another function is still said."""
    body = (
        "static inline void twice(char * owned maybe p)\n{\n  free(p);\n  unsafe {\n    free(p);\n  }\n}\n"
        "void f(void)\n{\n  twice(malloc(1));\n}\n" + TWICE.replace("void f(", "void h(")
    )
    proc = _compile(tmp_path, body, opt)
    assert [(l, k, m) for l, k, m in _diags(proc)] == [(16, "error", TWICE_MSG)], proc.stderr


def test_a_name_unsafe_wins(tmp_path):
    body = "int f(int unsafe)\n{\n  return unsafe + 1;\n}\nint unsafe2(void) { int unsafe = 2; return unsafe; }\n"
    proc = _compile(tmp_path, body)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_regular_c_has_no_keyword(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("int unsafe(int x) { return x; }\nint f(void) { return unsafe(1); }\n")
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-Wno-unused-parameter", "-Wno-shadow", "-Wno-cross-tu", "-Wno-tainted-format", "-ccompat", "-c", str(src), "-o", str(tmp_path / "t.o")], capture_output=True,
                          text=True)
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


def test_error_inside_the_region_ends_it(tmp_path):
    """An error recovered from inside unsafe { } does not leave the checks
    off for the rest of the unit."""
    body = (
        "void f(void)\n{\n  unsafe {\n    int x = ;\n  }\n}\n" + TWICE.replace("void f(", "void h(")
    )
    proc = _compile(tmp_path, body)
    assert proc.returncode != 0
    assert (11, "error", TWICE_MSG) in _diags(proc), proc.stderr


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_code_unchanged(tmp_path, opt):
    """unsafe changes what is said, not the code."""
    plain = UNSAFE_BLOCK.replace("  unsafe {\n    free(p);\n  }\n", "  {\n    free(p);\n  }\n")
    a = _compile(tmp_path, UNSAFE_BLOCK, opt)
    oa = (tmp_path / "t.o").read_bytes()
    b = _compile(tmp_path, plain, opt, "-ccompat", "-std=yasc")
    ob = (tmp_path / "t.o").read_bytes()
    assert a.returncode == 0 and b.returncode == 0, a.stderr + b.stderr
    assert oa == ob


# ---- S2: a regular-C pointer becomes an annotated one only in unsafe ----

C_DECLS = "#pragma tcc c_begin\nchar *c_find(const char *s);\nvoid c_keep(char *p);\n#pragma tcc c_end\n"
FROM_C = "a pointer from regular C becomes %s here: write unsafe(...)"


@pytest.mark.parametrize(
    "body,line,what",
    [
        ("int f(const char * view s)\n{\n  char * view a = c_find(s);\n  return *a;\n}\n", 3, "a view"),
        ("char * owned f(const char * view s)\n{\n  return c_find(s);\n}\n", 3, "owned"),
        ("int f(const char * view s)\n{\n  return use(c_find(s));\n}\n", 3, "a view"),
    ],
    ids=["initializer", "return", "argument"],
)
def test_regular_c_pointer_needs_unsafe(tmp_path, body, line, what):
    proc = _compile(tmp_path, C_DECLS + body)
    assert proc.returncode != 0
    assert (line + 4, "error", FROM_C % what) in _diags(proc), proc.stderr


def test_unsafe_vouches_for_it(tmp_path):
    body = "int f(const char * view s)\n{\n  char * view a = unsafe(c_find(s));\n  return *a;\n}\n"
    proc = _compile(tmp_path, C_DECLS + body)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_what_needs_no_unsafe(tmp_path):
    """Null (also `(T *)0` from a C macro), a regular-C variable holding it,
    and an annotated pointer handed to C."""
    body = (
        "#pragma tcc c_begin\n#define C_NONE ((char *)0)\n#pragma tcc c_end\n"
        "int f(inout char * view v)\n{\n  char * view maybe a = nullptr;\n  char * view maybe b = C_NONE;\n"
        "  c_keep(v);\n  return a == b;\n}\n"
    )
    proc = _compile(tmp_path, C_DECLS + body)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_ccompat_converts_silently(tmp_path):
    body = "int f(const char * view s)\n{\n  char * view a = c_find(s);\n  return *a;\n}\n"
    proc = _compile(tmp_path, C_DECLS + body, "-ccompat", "-std=yasc")
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


# ---- S2: explicit casts to owned / view / borrows pointers ----

CASTS = {
    "integer": ("unsigned * view r = (unsigned * view)0x40000000;", "a cast makes an integer a pointer: write unsafe(...)"),
    "from_c": ("char * view a = (char * view)c_find(s);", FROM_C % "a view"),
    "drops_maybe": ("char * view b = (char * view)m;", "a cast drops maybe: test the pointer, or write unsafe(...)"),
    "makes_owned": ("char * owned c = (char * owned)v;", "a cast makes a pointer that owns nothing owned: write unsafe(...)"),
    "drops_const": ("char * view d = (char * view)s;", "a cast drops const: write unsafe(...)"),
    "owned_to_view": ("char * view e = (char * view)o;", "a cast makes an owned pointer a view: write borrow(...)"),
}
CAST_FN = "void f(const char * view s, inout char * view maybe m, inout char * view v, char * owned o)\n{\n  %s\n  free(o);\n}\n"


@pytest.mark.parametrize("name", sorted(CASTS))
def test_cast_needs_unsafe(tmp_path, name):
    stmt, msg = CASTS[name]
    proc = _compile(tmp_path, C_DECLS + CAST_FN % stmt)
    assert proc.returncode != 0
    assert (7, "error", msg) in _diags(proc), proc.stderr


@pytest.mark.parametrize("name", sorted(CASTS))
def test_cast_in_unsafe(tmp_path, name):
    stmt = CASTS[name][0]
    lhs, rhs = stmt.rstrip(";").split(" = ", 1)
    proc = _compile(tmp_path, C_DECLS + CAST_FN % f"{lhs} = unsafe({rhs});")
    assert not any("cast" in m or "regular C" in m for _, _, m in _diags(proc)), proc.stderr


def test_casts_that_need_nothing(tmp_path):
    body = (
        "int f(char * view v)\n{\n  char buf[4] = {0};\n  char * view maybe z = (char * view maybe)nullptr;\n"
        "  const char * view c = (const char * view)v;\n  char * view b = (char * view)buf;\n"
        "  return (z != nullptr) + *c + *b;\n}\n"
    )
    proc = _compile(tmp_path, body)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


# ---- S2: memory through a regular-C pointer ----

DEREF = "a pointer from regular C is dereferenced: write unsafe { } around it"


@pytest.mark.parametrize(
    "expr",
    ["*c_find(s)", "c_find(s)[1]", "c_node()->v"],
    ids=["star", "index", "member"],
)
def test_deref_of_regular_c_pointer(tmp_path, expr):
    decls = C_DECLS.replace("#pragma tcc c_end", "struct cn { int v; };\nstruct cn *c_node(void);\n#pragma tcc c_end")
    body = f"int f(const char * view s)\n{{\n  return {expr};\n}}\n"
    proc = _compile(tmp_path, decls + body)
    assert proc.returncode != 0
    assert (9, "error", DEREF) in _diags(proc), proc.stderr
    ok = _compile(tmp_path, decls + body.replace(f"return {expr};", f"return unsafe({expr});"))
    assert ok.returncode == 0 and _diags(ok) == [], ok.stderr


def test_c_macro_and_function_pointer_need_nothing(tmp_path):
    """A dereference a regular-C macro expands to is regular C; a call
    through a regular-C function pointer is not memory."""
    decls = (
        "#pragma tcc c_begin\nextern int *c_errno(void);\n#define C_ERRNO (*c_errno())\n"
        "typedef int (*c_fn)(int);\nc_fn c_get(void);\n#pragma tcc c_end\n"
    )
    body = "int f(void)\n{\n  c_fn g = c_get();\n  return C_ERRNO + g(1);\n}\n"
    proc = _compile(tmp_path, decls + body)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_unsafe_report(tmp_path, opt):
    """-funsafe-report: a line per region, once even for a static inline
    whose body is parsed more than once (-O1+)."""
    body = (
        "static inline void twice(char * owned maybe p)\n{\n  free(p);\n  unsafe {\n    free(p);\n  }\n}\n"
        "void f(void)\n{\n  twice(malloc(1));\n  twice(malloc(1));\n  char * owned maybe q = malloc(1);\n"
        "  free(q);\n  unsafe(free(q));\n}\n"
    )
    proc = _compile(tmp_path, body, opt, "-funsafe-report")
    notes = sorted(int(l.split(":")[1]) - PRELUDE_LINES for l in proc.stderr.splitlines() if "note: unsafe region" in l)
    assert proc.returncode == 0 and notes == [4, 14], proc.stderr
    assert "unsafe region" not in _compile(tmp_path, body, opt).stderr


def test_offset_from_regular_c_stays_unchecked(tmp_path):
    body = "int f(const char * view s)\n{\n  char * view a = c_find(s) + 1;\n  return *a;\n}\n"
    proc = _compile(tmp_path, C_DECLS + body)
    assert proc.returncode != 0
    assert (7, "error", FROM_C % "a view") in _diags(proc), proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_same_variable_after_the_region(tmp_path, opt):
    body = "void f(void)\n{\n  char * owned maybe p = malloc(1);\n  free(p);\n  unsafe {\n    free(p);\n  }\n  free(p);\n}\n"
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode != 0
    assert [(l, k, m) for l, k, m in _diags(proc)] == [(8, "error", TWICE_MSG)], proc.stderr


def test_w_does_not_hide_strict_errors(tmp_path):
    """-w hides warnings; the strict lifetime errors are errors still."""
    proc = _compile(tmp_path, TWICE, "-w")
    assert proc.returncode != 0
    assert (5, "error", TWICE_MSG) in _diags(proc), proc.stderr
    plain = _compile(tmp_path, "int f(char * view p)\n{\n  int x = 0;\n  return p[0];\n}\n", "-w")
    assert plain.returncode == 0 and plain.stderr == "", plain.stderr
    # yasc enforces initialization (test_yasc_init.py): an error -w keeps
    uninit = _compile(tmp_path, "int f(char * view p)\n{\n  int x;\n  return x + p[0];\n}\n", "-w")
    assert uninit.returncode != 0 and (4, "error", "'x' is used uninitialized") in _diags(uninit), uninit.stderr


# ---- unsafe works by lines: a region has its lines to itself ----

LINE_RULES = {
    "code_before_block": (
        "void f(char * owned maybe p)\n{\n  use(\"x\"); unsafe {\n    free(p);\n  }\n}\n", 3,
        "unsafe { } must begin its line: put what comes before it on the line above"),
    "code_after_block": (
        "void f(char * owned maybe p)\n{\n  unsafe {\n    free(p);\n  } use(\"y\");\n}\n", 5,
        "unsafe { } must end its line: put what follows its '}' on the next line"),
    "statement_after_expr": (
        "void f(char * owned maybe p)\n{\n  free(unsafe(p)); use(\"z\");\n}\n", 3,
        "unsafe(...) takes its whole line: put this statement on a line of its own"),
    "statement_before_expr": (
        "void f(char * owned maybe p)\n{\n  use(\"w\"); free(unsafe(p));\n}\n", 3,
        "unsafe(...) takes its whole line: put what comes before its statement on the line above"),
}


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
@pytest.mark.parametrize("name", sorted(LINE_RULES))
def test_unsafe_has_its_lines(tmp_path, name, opt):
    body, line, msg = LINE_RULES[name]
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode != 0
    assert (line, "error", msg) in _diags(proc), proc.stderr


def test_unsafe_lines_are_per_file(tmp_path):
    """The line of an unsafe(...) is its file's: another unit compiled in the
    same command, on the same line number, is not touched."""
    (tmp_path / "a.c").write_text(PRELUDE + "void f(char * owned maybe p)\n{\n  free(unsafe(p));\n}\n")
    (tmp_path / "b.c").write_text("int g(int x);\nint h(int x)\n{\n  int y = x;\n  y++; return g(y);\n}\n" * 1)
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-Wno-unused-parameter", "-Wno-shadow", "-Wno-cross-tu", "-Wno-tainted-format", "-c", "a.c"], capture_output=True, text=True, cwd=tmp_path)
    assert proc.returncode == 0, proc.stderr
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-Wno-unused-parameter", "-Wno-shadow", "-Wno-cross-tu", "-Wno-tainted-format", "-ccompat", "-c", "a.c", "b.c"], capture_output=True, text=True, cwd=tmp_path)
    assert "unsafe" not in proc.stderr, proc.stderr
