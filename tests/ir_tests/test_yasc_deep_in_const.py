"""yasc: an in parameter (the default: no inout / out / drop, a pointer that is
a view) reads, and reads only, everything it reaches: every level it points to
is const, not just the first.  `char **pp` is `const char *const * pp`; a
caller still passes its `char **` (adding const at every level is safe).
inout / out write the first level; the levels under it stay as written.

So `const` on any level an in parameter points to says nothing:
-Wredundant-const (in the safe default) says so, with a fix-it per `const`.
Not on the parameter itself (`char *const p`, a by-value `const int n`), not
on inout / out / drop / owned parameters, not where a typedef brings it.

tcc -yasc-c-header writes the const an in parameter lacks on the first level
it points to -- C converts `char **` to `char *const *` and no further -- so a
yasc header drops its redundant const and the C header stays what it was.
__yl_c_api(n, T) says the whole yasc type (every level const).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

READ_ONLY = "assignment of read-only location (a view parameter's target is written only when it says inout or out)"
REDUNDANT = "a parameter's target is read-only, every level of it, unless it says inout or out: drop 'const'"


def _run(tmp_path, body, *flags, yasc=True):
    src = tmp_path / "t.c"
    src.write_text(("#pragma yasc\n" if yasc else "") + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-fsyntax-only", *flags, str(src)],
        capture_output=True, text=True, cwd=tmp_path,
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - 1, parts[1], parts[2]))
    return out


def _fixits(proc):
    """(line, first column, end column, text) of each fix-it, lines of the body"""
    out = []
    for line in proc.stderr.splitlines():
        if line.startswith("fix-it:"):
            span = line.split(":{", 1)[1].split("}", 1)[0]
            a, b = span.split("-")
            out.append((int(a.split(":")[0]) - 1, int(a.split(":")[1]), int(b.split(":")[1])))
    return sorted(out)


# ---------------------------------------------------------------- deep in

WRITES = {
    "second_level": "int f(char **pp) { **pp = 'a'; return 0; }\n",
    "second_level_index": "int f(char **pp) { pp[1][2] = 'a'; return 0; }\n",
    "first_level": "int f(char **pp) { *pp = pp[1]; return 0; }\n",
    "array_element": "int f(char *view a[]) { a[0][0] = 'a'; return 0; }\n",
    "struct_through_two": "struct s { int x; };\nint f(struct s **pp) { (*pp)->x = 1; return 0; }\n",
    "third_level": "int f(int ***p) { ***p = 1; return 0; }\n",
    "typedef_level": "typedef char *view str;\nint f(str *p) { (*p)[0] = 'a'; return 0; }\n",
}


@pytest.mark.parametrize("name", sorted(WRITES))
def test_an_in_parameter_writes_nothing_it_reaches(tmp_path, name):
    proc = _run(tmp_path, WRITES[name])
    assert proc.returncode != 0
    assert [d[1:] for d in _diags(proc)] == [("error", READ_ONLY)], proc.stderr


def test_a_copy_of_an_inner_level_keeps_its_const(tmp_path):
    # a plain local takes the read-only value; writing through it is the error
    proc = _run(tmp_path, "int f(char **pp) { char *p = pp[0]; p[0] = 'x'; return 0; }\n")
    assert proc.returncode != 0
    assert [d[1:] for d in _diags(proc)] == [
        ("error", "writing through 'p', which may hold a read-only value (a const one was stored in it)")], proc.stderr
    # read through a const copy: fine
    proc = _run(tmp_path, "int f(char **pp) { const char *p = pp[0]; return p[0]; }\n")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


ALLOWED = {
    "inout_both_levels": "int f(inout char **pp) { **pp = 'a'; *pp = pp[1]; return 0; }\n",
    "out_first_level": "void f(out char *? *pp) { *pp = nullptr; }\n",
    "inout_array": "void f(inout char *view a[]) { a[0][0] = 'a'; }\n",
    "reads": "int f(char **pp, int ***q) { return pp[0][0] + ***q; }\n",
    "callback": "int f(int (*cb)(int)) { return cb(1); }\n",
}


@pytest.mark.parametrize("name", sorted(ALLOWED))
def test_inout_out_and_reads_are_fine(tmp_path, name):
    proc = _run(tmp_path, ALLOWED[name])
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_callers_pass_what_they_have(tmp_path):
    body = (
        "int take(char **pp);\n"
        "int take3(int ***p);\n"
        "int f(inout char **argv, inout int ***p) { return take(argv) + take3(p); }\n"
        "int main(int argc, char **argv) { return take(argv) + argc; }\n"
    )
    proc = _run(tmp_path, body, "-Wno-param-access")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_regular_c_is_left_alone(tmp_path):
    proc = _run(tmp_path, "int f(char **pp) { **pp = 'a'; *pp = 0; return 0; }\n", "-ccompat", yasc=False)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


# -Wparam-access: a write to a deeper level is a write -- dropping inout
# there would make it an error now
DEEP_WRITES = {
    "second_level": "void f(inout char **pp) { **pp = 1; }\n",
    "index": "void g(inout char **pp) { pp[0][1] = 1; }\n",
    "through_a_local": "void h(inout char **pp) { char *p = pp[0]; p[0] = 1; }\n",
    "to_an_out_parameter": "void cp(out char *d, char *s);\nvoid m(inout char **pp) { cp(pp[0], \"x\"); }\n",
    "third_level": "int n(inout int ***p) { ***p = 1; return 0; }\n",
    "struct_through_two": "struct s { int x; };\nvoid q(inout struct s **pp) { (*pp)->x = 1; }\n",
}
PARAM_ACCESS = "is inout, but"


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
@pytest.mark.parametrize("name", sorted(DEEP_WRITES))
def test_a_deeper_write_keeps_inout(tmp_path, name, opt):
    proc = _run(tmp_path, DEEP_WRITES[name], opt)
    assert proc.returncode == 0, proc.stderr
    assert PARAM_ACCESS not in proc.stderr, proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_reads_and_member_targets_still_say_drop_inout(tmp_path, opt):
    # only read; a member's target (not const through the struct's const) written
    body = ("int r(inout char **pp) { return pp[0][0]; }\n"
            "struct t { char *b; };\nvoid k(inout struct t *x) { x->b[0] = 1; }\n")
    proc = _run(tmp_path, body, opt)
    assert proc.stderr.count("writes nothing through it: drop inout") == 2, proc.stderr
    # and dropping it is right: no error
    assert _run(tmp_path, body.replace("inout ", ""), opt).returncode == 0


# ---------------------------------------------------------------- -Wredundant-const

FIXITS = {
    # body, the (line, first column, end column) of each fix-it
    "base": ("int a(const char *s);\n", [(1, 7, 13)]),
    "after_type": ("int b(char const *s);\n", [(1, 12, 18)]),
    "first_level": ("int c(char *const *pp);\n", [(1, 13, 19)]),
    "both_levels": ("int d(const char *const *pp);\n", [(1, 7, 13), (1, 19, 25)]),
    "array": ("int e(const char *view argv[]);\n", [(1, 7, 13)]),
    "typedef_pointer": ("typedef char *view str;\nint g(const str *p);\n", [(2, 7, 13)]),
    "abstract": ("int h(const char *, int);\n", [(1, 7, 13)]),
    "definition": ("int k(const char *s) { return s[0]; }\n", [(1, 7, 13)]),
    "two_parameters": ("int m(const char *a, const char *b);\n", [(1, 7, 13), (1, 22, 28)]),
    "callback_parameters": ("int n(int (*cmp)(const void *, const void *));\n", [(1, 18, 24), (1, 32, 38)]),
    "maybe": ("int p(const char *?s);\n", [(1, 7, 13)]),
    "struct": ("struct box { int n; };\nint q(const struct box *b);\n", [(2, 7, 13)]),
}


@pytest.mark.parametrize("name", sorted(FIXITS))
def test_redundant_const_warns_with_a_fixit(tmp_path, name):
    body, fixits = FIXITS[name]
    proc = _run(tmp_path, body, "-fdiagnostics-parseable-fixits")
    assert proc.returncode == 0, proc.stderr
    diags = _diags(proc)
    assert diags and all(d[1:] == ("warning", REDUNDANT) for d in diags), proc.stderr
    assert _fixits(proc) == fixits, proc.stderr


@pytest.mark.parametrize("name", sorted(FIXITS))
def test_the_fixit_leaves_nothing_to_say(tmp_path, name):
    """Applying the fix-its: no warning, and the same object."""
    body, fixits = FIXITS[name]
    lines = body.split("\n")
    for line, a, b in sorted(fixits, reverse=True):
        lines[line - 1] = lines[line - 1][: a - 1] + lines[line - 1][b - 1:]
    proc = _run(tmp_path, "\n".join(lines))
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


QUIET = {
    "own_pointer": "int a(char *const p);\n",
    "by_value": "int b(const int n);\n",
    "inout": "void c(inout const char **pp);\n",
    "out": "void d(char *s, out const char *borrows(s) *e);\n",
    "owned": "void e(const char *owned p);\n",
    "drop": "void free(drop void *?owned p);\nvoid f(drop const char *?owned p);\n",
    "typedef_const": "typedef const char *view cstr;\nint g(cstr s);\n",
    "no_const": "int h(char *s, char **pp, int (*cmp)(void *, void *));\n",
    "local_and_result": "const char *borrows(s) k(char *s) { const char *p = s; return p; }\n",
    "member": "struct m { const char *name; };\n",
    "c_section": "#pragma tcc c_begin\nint l(const char *s);\n#pragma tcc c_end\n",
}


@pytest.mark.parametrize("name", sorted(QUIET))
def test_redundant_const_is_quiet_where_const_says_something(tmp_path, name):
    proc = _run(tmp_path, QUIET[name])
    assert not [d for d in _diags(proc) if d[2] == REDUNDANT], proc.stderr


def test_redundant_const_not_in_regular_c_or_when_turned_off(tmp_path):
    proc = _run(tmp_path, "int a(const char *s);\n", "-ccompat", yasc=False)
    assert not proc.stderr, proc.stderr
    proc = _run(tmp_path, "int a(const char *s);\n", "-Wno-redundant-const")
    assert not proc.stderr, proc.stderr
    proc = _run(tmp_path, "int a(const char *s);\n", "-Werror=redundant-const")
    assert proc.returncode != 0


# ---------------------------------------------------------------- the C header

YASC_HEADER = """\
#pragma yasc
#pragma once
#include <sys/_attrs.h>
typedef struct ctr ctr_t;
int execv(char *path, char *@(view, c_api(char * const)) argv[]);
int execve(char *path, char *argv[], char *envp[]);
int g(char **pp);
int h(const char *s, char *t, inout char *u, out char *w, char *owned o, int n);
unsigned len(char *);
int c(ctr_t *c, unused ctr_t *d);
void qs(void *b, unsigned n, int (*cmp)(void *, void *));
typedef int cmp_t(void *, void *);
typedef int (*cmpp_t)(void *, void *);
char *?borrows(s) chr(char *s, int c);
long sl(char *s, out const char *@(borrows(s), c_api(char *)) *?endptr, int base);
int m(char *?s, char **v);
int setgroups(unsigned size, unsigned list[?size]);
struct ops { int (*f)(char *s); };
"""

C_DECLS = [
    "int execv(const char *path, char * const argv[]) __yl_c_api(2, const char * const []);",
    "int execve(const char *path, char *const argv[], char *const envp[]);",
    "int g(char *const *pp);",
    "int h(const char *s, const char *t, char *u, char *w, char *o, int n);",
    "unsigned len(const char *);",
    "int c(const ctr_t *c, const ctr_t *d);",
    "void qs(const void *b, unsigned n, int (*cmp)(const void *, const void *));",
    "typedef int cmp_t(const void *, const void *);",
    "typedef int (*cmpp_t)(const void *, const void *);",
    "char *chr(const char *s, int c);",
    "long sl(const char *s, char **endptr, int base) __yl_c_api(2, const char * *);",
    "int m(const char *s, char *const *v);",
    "int setgroups(unsigned size, const unsigned *list) __yl_access(read_only, 2, 1);",
    "struct ops { int (*f)(const char *s); };",
]


def _cheader(tmp_path, text):
    inc = tmp_path / "inc"
    (inc / "yasc").mkdir(parents=True, exist_ok=True)
    (inc / "sys").mkdir(exist_ok=True)
    (inc / "sys" / "_attrs.h").write_text("#pragma once\n#define __yl_c_api(n, ...)\n#define __yl_access(mode, ref, size)\n")
    (inc / "yasc" / "x.h").write_text(text)
    proc = subprocess.run([str(TCC), "-yasc-c-header", "yasc/x.h"], cwd=inc, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return proc.stdout


def test_c_header_says_the_const_of_in_parameters(tmp_path):
    out = _cheader(tmp_path, YASC_HEADER)
    for decl in C_DECLS:
        assert decl in out, (decl, out)


def test_c_header_is_the_same_without_the_redundant_const(tmp_path):
    written = (
        "#pragma yasc\n#pragma once\n#include <sys/_attrs.h>\n"
        "unsigned strlen(const char *s);\nint strcmp(const char *a, const char *b);\n"
        "int execve(const char *path, char *const argv[], char *const envp[]);\n"
        "void qsort(inout void *b, unsigned n, unsigned sz, int (*cmp)(const void *, const void *));\n"
    )
    dropped = (
        "#pragma yasc\n#pragma once\n#include <sys/_attrs.h>\n"
        "unsigned strlen(char *s);\nint strcmp(char *a, char *b);\n"
        "int execve(char *path, char *argv[], char *envp[]);\n"
        "void qsort(inout void *b, unsigned n, unsigned sz, int (*cmp)(void *, void *));\n"
    )
    assert _cheader(tmp_path, written) == _cheader(tmp_path, dropped)


def test_c_header_compiles_as_c_and_takes_c_callers(tmp_path):
    out = _cheader(tmp_path, YASC_HEADER)
    hdr = tmp_path / "inc" / "x_c.h"
    hdr.write_text(out)
    use = tmp_path / "use.c"
    use.write_text('#include "x_c.h"\n'
                   "int caller(char **argv, char *s, const char *k) { return g(argv) + (int)len(s) + (int)len(k) + m(s, argv); }\n")
    proc = subprocess.run([str(TCC), "-ccompat", "-Wall", "-fsyntax-only", "-I", str(tmp_path / "inc"), str(use)],
                          capture_output=True, text=True)
    assert proc.returncode == 0 and "warning" not in proc.stderr, proc.stderr
