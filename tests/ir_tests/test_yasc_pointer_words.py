"""A yasc pointer's words go against its '*' (docs/yasc_cheatsheet.md).

`*owned`, `*view`, `*borrows(s)`: no space between the '*' and its word.
`?` (may be null; it was the word `maybe`) comes first: `*?owned`, `*?`.
One word per '*'; two or more in `@( )`: `*@(const, owned)`.  So each level
of a declaration is one piece, and the spaces are between levels:
`char *?borrows(s) *?borrows(c) p`.  An int handle takes `?` before its
word (`int ?owned(close) fd`), a parameter array's brackets after the '['
(`int a[?4]`).  The lexer reads these (tccpp.c yasc_glue), so a `?` that is
a conditional's and `a * @min(x, y)` stay what they were.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = "#pragma yasc\nvoid release(drop void *?owned p);\nint close(int drop ?owned(close) fd);\n"


def _run(tmp_path, src, *flags, name="t.c"):
    (tmp_path / name).write_text(src)
    return subprocess.run(
        [str(TCC), "-nostdinc", "-Wno-missing-prototypes", "-Wno-unused-parameter", "-Wno-unused-variable",
         "-fsyntax-only", *flags, str(tmp_path / name)],
        capture_output=True,
        text=True,
    )


GOOD = {
    "owned_maybe_result": "char *?owned make(void);\n",
    "two_levels": "void two(char *s, out char *?borrows(s) *?end);\n",
    "owned_array_of_owned": "void drop_all(drop char *?owned *owned v);\n",
    "group": "struct box { char *@(const, owned) name; };\n",
    "group_maybe": "char *?@(owned, takes(p)) keep(char *?owned p);\n",
    "group_list_args": "const char *?@(borrows(static), c_api(char *)) last_error(void);\n",
    "bare_maybe_param": "int len(char *?s) { return s ? 1 : 0; }\n",
    "bare_maybe_local": "int f(char *s) { char *?q = s; return q != nullptr; }\n",
    "compound_literal": "int f(void) { const char *?view *v = (const char *?[]){\"a\", nullptr}; return v[1] == nullptr; }\n",
    "brackets": "int first(int a[?4]) { return a ? a[0] : 0; }\n",
    "brackets_view": "int count(char *view a[?view]);\n",
    "handle": "int f(int ?owned(close) fd) { return fd < 0 ? -1 : close(fd); }\n",
    "handle_view": "int g(int ?view fd);\n",
    "out_borrows": "long sl(char *s, out char *borrows(s) *?end);\n",
    "const_glued": "int f(char *const p);\n",
    "const_as_c_writes_it": "int f(char * const p, char * const volatile q, char *const volatile r);\n",
    "conditional": "int sel(int c, int a, int b) { return c ?a : b; }\n",
    "times": "int mul(int a, int b) { return a * b; }\n",
    "maybe_is_a_name": "int maybe = 1;\nint f(void) { return maybe ? 2 : 3; }\n",
    "pointer_named_maybe": "int f(char *s) { char *maybe = s; return *maybe; }\n",
    "macro_spacing": "#define OWN(T) T * owned\nvoid take(drop OWN(char) p) { release(p); }\n",
    "cast": "int f(inout char *?p) { return cast<as(char *?)>(p) == nullptr; }\n",
}


@pytest.mark.parametrize("name", sorted(GOOD))
def test_accepted(tmp_path, name):
    proc = _run(tmp_path, PRELUDE + GOOD[name])
    assert proc.returncode == 0 and "error" not in proc.stderr, proc.stderr


BAD = {
    "space_after_star": ("char * owned make(void);\n", "a pointer's word goes against its '*': write '*owned'"),
    "space_after_maybe": ("char *? owned make(void);\n", "a pointer's word goes against its '*?': write '*?owned'"),
    "space_before_maybe": ("int f(char * ?s);\n", "'?' goes against its '*': write '*?'"),
    "const_then_word": ("char * const owned p;\n", "a pointer's words go in @( ) with its C qualifiers: write '*@(const, owned)'"),
    "two_words": ("char *const owned make(void);\n", "one word against a '*': write '*@(const, owned)'"),
    "word_then_const": ("char *owned const p;\n", "one word against a '*': write '*@(owned, const)'"),
    "word_after_group": ("char *@(const, owned) view make(void);\n", "one word against a '*': write 'view' in its @( )"),
    "group_spaced": ("char *@ (const, owned) make(void);\n", "a pointer's words: '*@(a, b)', the '(' against the '@'"),
    "maybe_in_group": ("char *@(?, owned) make(void);\n", "'?' in *@( ): write it against the '*', before the '@'"),
    "maybe_gone": ("char * maybe p;\n", "'maybe' is gone: write '?' against the '*'"),
    "maybe_gone_brackets": ("int f(int a[maybe 4]);\n", "'maybe' is gone: write '?' against the '['"),
    "maybe_gone_handle": ("int f(int owned(close) maybe fd);\n",
                          "'maybe' is gone: write '?' against a pointer's '*' ('*?owned') or before a handle's word"),
    "owned_and_view": ("char *@(owned, view) p;\n", "a pointer is owned, a view or borrows(...): one of them"),
}


@pytest.mark.parametrize("name", sorted(BAD))
def test_rejected(tmp_path, name):
    src, msg = BAD[name]
    proc = _run(tmp_path, PRELUDE + src)
    assert proc.returncode != 0 and msg in proc.stderr, proc.stderr


def test_at_builtin_after_a_star(tmp_path):
    """`a * @min(x, y)` is a builtin's call, not a pointer's words"""
    proc = _run(tmp_path, PRELUDE + "int f(int a, int x, int y) { return a * @min(x, y); }\n")
    assert proc.returncode == 0, proc.stderr


def test_regular_c_is_not_held_to_it(tmp_path):
    """a C section (and a C file) keeps C's spacing: `char * const p`"""
    src = PRELUDE + "#pragma tcc c_begin\nint c_f(char * const p);\n#pragma tcc c_end\n"
    assert _run(tmp_path, src).returncode == 0
    proc = subprocess.run([str(TCC), "-nostdinc", "-ccompat", "-fsyntax-only", "-x", "c", "-"],
                          input="int g(char * const p, char * volatile q);\n", capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr


def test_types_print_the_glued_form(tmp_path):
    proc = _run(tmp_path, PRELUDE + "struct S { int a; };\nchar *?owned make(void);\nvoid f(void) { struct S s; s = make(); }\n")
    assert "'char *?owned'" in proc.stderr, proc.stderr
    proc = _run(tmp_path, PRELUDE + "struct S { int a; };\nstruct box { char *@(const, owned) n; };\n"
                "void f(inout struct box *b) { struct S s = {}; s = b->n; (void)s; }\n")
    assert "'char *@(const, owned)'" in proc.stderr, proc.stderr


def test_preprocess_keeps_the_spelling(tmp_path):
    """tcc -E writes what it reads: its output compiles the same"""
    src = PRELUDE + "#define OWN(T) T *?owned\nint f(char *?@(const, view) s, OWN(char) q) { release(q); return s ? 1 : 0; }\n"
    (tmp_path / "t.c").write_text(src)
    pp = subprocess.run([str(TCC), "-nostdinc", "-E", str(tmp_path / "t.c")], capture_output=True, text=True)
    assert pp.returncode == 0, pp.stderr
    assert "char *?@(const, view) s, char *?owned q" in pp.stdout, pp.stdout
    proc = _run(tmp_path, pp.stdout, "-Wno-redundant-view", name="e.c")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_c_header_drops_the_words(tmp_path):
    inc = tmp_path / "yasc"
    inc.mkdir()
    (inc / "w.h").write_text("#pragma yasc\nchar *?owned make(char *?@(const, view) s, int a[?4]);\n"
                             "int close_it(int drop ?owned(close) fd);\n")
    proc = subprocess.run([str(TCC), "-yasc-c-header", str(inc / "w.h")], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    out = proc.stdout
    assert "char *make(" in out.replace("char * make(", "char *make("), out
    assert "?" not in out.split("#ifndef __YASC__")[-1] if "#ifndef __YASC__" in out else "?" not in out.split("*/", 1)[-1], out
    assert "@" not in out.split("*/", 1)[-1], out


FIXITS = {
    # a word that says nothing goes: alone it leaves the '*', in a group the
    # group is written again (one word left stands against the '*')
    "alone": ("int f(char *view s);\n", ':{%d:13-%d:18}:""'),
    "after_maybe": ("int f(char *?view s);\n", ':{%d:14-%d:19}:""'),
    "abstract": ("int f(char *view);\n", ':{%d:13-%d:17}:""'),
    "group_to_one": ("int f(char *?@(const, view) s);\n", ':{%d:14-%d:28}:"const"'),
    "group_to_two": ("int f(char *@(view, restrict, const) s);\n", ':{%d:13-%d:37}:"@(restrict, const)"'),
    "const_in_group": ("int e(char *@(view, const) argv[]);\n", ':{%d:13-%d:27}:"view"'),
}


@pytest.mark.parametrize("name", sorted(FIXITS))
def test_redundant_word_fixit(tmp_path, name):
    src, fix = FIXITS[name]
    line = 2
    proc = _run(tmp_path, "#pragma yasc\n" + src, "-fdiagnostics-parseable-fixits")
    assert proc.returncode == 0, proc.stderr
    assert fix % (line, line) in proc.stderr, proc.stderr
