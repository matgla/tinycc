"""What a yasc parameter does with what it points to, said before its type:

    void f(T *p);         // in (the default): *p is read-only
    void f(in T *p);      // the same, said
    void f(inout T *p);   // *p is read and written
    void f(out T *p);     // *p is written before it is read

A view parameter's target is const unless the parameter says inout or out
(drop consumes it); owned pointers, int handles and regular-C pointers are
not views and are unchanged.  Writing the read-only target is an error in
yasc, and so is handing it on where something may write it.  `in` is a
keyword only where a parameter's declaration starts.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

Y = "#pragma yasc\nvoid free(drop void * owned maybe p);\nstruct box { int n; char * owned maybe s; };\n"
RO = "assignment of read-only location"


def _run(tmp_path, src, *flags):
    (tmp_path / "t.c").write_text(src)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-unused-parameter", "-c", *flags, str(tmp_path / "t.c"), "-o",
         str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


CLEAN = {
    "read_in": "int f(int *p) { return *p; }\n",
    "read_in_said": "int f(in int *p) { return *p; }\n",
    "write_inout": "void f(inout int *p) { *p = *p + 1; }\n",
    "write_out": "void f(out int *p) { *p = 1; }\n",
    "member_inout": "void f(inout struct box *b) { b->n = 1; }\n",
    "array_out": "void f(out char a[2]) { a[0] = 0; a[1] = 0; }\n",
    "inout_to_inout": "void g(inout int *p);\nvoid f(inout int *p) { g(p); }\n",
    "in_to_in": "int g(int *p);\nint f(int *p) { return g(p); }\n",
    "owned_writable": "void f(char * owned p) { p[0] = 0; free(p); }\n",
    "handle_untouched": "int f(int view fd) { return (int)fd; }\n",
    # `in` is a name everywhere but where a parameter's declaration starts
    "in_as_name": "int f(int in) { int out_ = in; return out_; }\nint g(void) { int in = 2; return in; }\n",
    "free_member_inout": "void f(inout struct box *b) { free(b->s); b->s = nullptr; }\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_clean(tmp_path, name, opt):
    proc = _run(tmp_path, Y + CLEAN[name], opt)
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


ERRORS = {
    "write_in": ("void f(int *p) { *p = 1; }\n", RO),
    "write_member_in": ("void f(struct box *b) { b->n = 1; }\n", RO),
    "write_array_in": ("void f(char a[2]) { a[0] = 0; }\n", RO),
    "in_to_inout": ("void g(inout int *p);\nvoid f(int *p) { g(p); }\n", "discards 'const'"),
    "return_in_writable": ("int * view f(int *p) { return p; }\n", "discards 'const'"),
    "both_words": ("void f(in inout int *p);\n", "a parameter is in, inout or out: one of them"),
    "inout_owned": ("void f(inout char * owned p);\n", "inout: only a view parameter's target"),
    "inout_int": ("void f(inout int n);\n", "inout: only a view parameter's target"),
    "redecl": ("void f(inout int *p);\nvoid f(int *p) { (void)p; }\n",
               "differs in in / inout (whether its target is written)"),
    # a read-only parameter cannot give the member a new value
    "free_member_in": ("void f(struct box *b) { free(b->s); }\n",
                       "'b->s' is freed when 'f' returns: 'b' still owns it, and its target is read-only -- say "
                       "inout to give it a new value or null"),
}


@pytest.mark.parametrize("name", sorted(ERRORS))
def test_errors(tmp_path, name):
    src, msg = ERRORS[name]
    proc = _run(tmp_path, Y + src)
    assert proc.returncode != 0
    assert msg in proc.stderr, proc.stderr


def test_regular_c_unchanged(tmp_path):
    # regular C (-ccompat, no pragma): a pointer parameter is what C says
    proc = _run(tmp_path, "void f(int *p) { *p = 1; }\nint in(int out) { return out; }\n", "-ccompat")
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


def test_c_header_spells_in_as_const(tmp_path):
    """-fmodule-header: an `in` parameter's target is const in C too, an
    inout / out one is not, and the yasc words are gone"""
    (tmp_path / "m.c").write_text(
        "#pragma yasc\nmodule io;\n"
        "export int get(int *p, inout int *q, out int *r, in const char *s);\n"
        "export int arr(int a[4], inout char b[4]);\n"
    )
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-c", str(tmp_path / "m.c"), "-o", str(tmp_path / "m.o"),
         f"-fmodule-header={tmp_path / 'inc'}"],
        capture_output=True, text=True, cwd=tmp_path,
    )
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    text = (tmp_path / "inc" / "io.h").read_text()
    assert "int get(int const * p, int * q, int * r, const char * s);" in text, text
    assert "int arr(int const a[4], char b[4]);" in text, text
    for w in ("inout", " in ", "out ", "view"):
        assert w not in text, (w, text)
