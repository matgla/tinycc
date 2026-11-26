"""A function declared again (or defined) in yasc says the same about its
pointers as the declaration before it: owned / view, maybe, out, drop,
takes(...), restrict and borrows(...) at every pointer level of the result
and of each parameter.  C's type compatibility leaves those words out, so a
header saying `char * view` and a definition saying `char * owned` (or
borrows(s) on one and not the other) went through, and callers that saw
only the header were checked against a contract the body does not keep.

A prototype written in regular C (a header without yasc) says nothing, and
a yasc declaration after it is no conflict.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")


# a header in regular C (no #pragma yasc): its pointers say nothing
C_HEADER = "char *dup(const char *s);\nlong sl(const char *s, char **e, int b);\n"


def _run(tmp_path, src, name="t.c"):
    (tmp_path / "c.h").write_text(C_HEADER)
    (tmp_path / name).write_text(src)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-unused-parameter", "-c", str(tmp_path / name), "-o",
         str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


Y = "#pragma yasc\n"

CONFLICTS = {
    "owned_view": (Y + "void g(char * owned p);\nvoid g(char *p) { }\n",
                   "'g' is declared again with another yasc contract: parameter 1 ('p') differs in owned / view"),
    "maybe": (Y + "void g(char * maybe p);\nvoid g(char *p) { }\n",
              "parameter 1 ('p') differs in maybe"),
    "out": (Y + "void h(out int *p);\nvoid h(int *p) { *p = 1; }\n", "parameter 1 ('p') differs in out"),
    "drop": (Y + "void free(drop void * owned maybe p);\nvoid free(void * owned maybe p);\n",
             "parameter 1 ('p') differs in drop"),
    "out_target_borrows": (
        Y + "void p(const char *s, out const char * view *e);\n"
        "void p(const char *s, out const char * borrows(s) *e) { *e = s; }\n",
        "parameter 2 ('e') differs in borrows(...)"),
    "result_borrows": (
        Y + "const char * borrows(s) r(const char *s, const char *t);\n"
        "const char * borrows(t) r(const char *s, const char *t) { return t; }\n",
        "the result differs in borrows(...)"),
    "result_owned": (Y + "char * owned maybe a(void);\nchar * borrows(static) maybe a(void);\n",
                     "the result differs in owned / view"),
    "unnamed": (Y + "void g(char * owned);\nvoid g(char *);\n", "parameter 1 differs in owned / view"),
    "restrict": (Y + "void g(char *p);\nvoid g(char *restrict p) { }\n", "parameter 1 ('p') differs in restrict"),
}


@pytest.mark.parametrize("name", sorted(CONFLICTS))
def test_conflict(tmp_path, name):
    src, msg = CONFLICTS[name]
    proc = _run(tmp_path, src)
    assert proc.returncode != 0
    assert msg in proc.stderr, proc.stderr


AGREE = {
    "same": Y + "void p(const char *s, out const char * borrows(s) *e);\n"
    "void p(const char *s, out const char * borrows(s) *e) { *e = s; }\n",
    # the elision names the same parameters as borrows(...) does
    "elision_and_explicit": Y + "char * view f(inout char *s, int n);\n"
    "char * borrows(s) f(inout char *s, int n) { return s + n; }\n",
    # in is the default: said or not, the same
    "in_said": Y + "int n(const char *s);\nint n(in char const *s) { return *s; }\n",
    # a regular-C prototype says nothing about ownership
    # a parameter's own pointer is a view, said or not
    "view_said_or_not": Y + "void g(char *p);\nvoid g(char * view p) { }\n",
    "regular_c_first": "#include \"c.h\"\n" + Y + "char * owned maybe dup(const char *s);\n"
    "long sl(const char *s, out char * borrows(s) * maybe e, int b);\n",
}


@pytest.mark.parametrize("name", sorted(AGREE))
def test_agree(tmp_path, name):
    proc = _run(tmp_path, AGREE[name])
    assert "yasc contract" not in proc.stderr
    assert proc.returncode == 0, proc.stderr
