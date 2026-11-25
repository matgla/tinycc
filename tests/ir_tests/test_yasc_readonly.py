"""yasc has no `readonly`.

`T * readonly p` was `const T * view p` (Rust's `&T`).  Since a view
parameter's target is read-only unless it says inout or out (`in`, the
default), it said nothing in a parameter, and elsewhere it only spelled
`const T * view` a second way.  The word is gone: where it would qualify a
pointer the compiler says so, and everywhere else it is a name, in yasc as
in C (toybox's sh.c has a variable of that name).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

GONE = ("readonly is gone: a view parameter's target is read-only already (inout or out to write it); "
        "elsewhere write 'const T * view'")


def _compile(tmp_path, src, *flags):
    (tmp_path / "t.c").write_text(src)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-unused-parameter", "-c", *flags, str(tmp_path / "t.c"), "-o",
         str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


ERRORS = {
    "param": "int f(char * readonly p) { return *p; }\n",
    "maybe": "int f(char * readonly maybe p);\n",
    "two_levels": "int main(int argc, char * readonly * readonly argv);\n",
    "array_param": "int f(char * readonly a[4]);\n",
    "brackets": "int f(char * view a[readonly]);\n",
    "nested": "void f(char (* readonly m)[4]);\n",
    "local": "int f(const char * view s) { char * readonly p = s; return *p; }\n",
    "member": "struct s { char * readonly name; };\n",
    "typedef": "typedef char * readonly str;\n",
}


@pytest.mark.parametrize("name", sorted(ERRORS))
def test_gone(tmp_path, name):
    proc = _compile(tmp_path, "#pragma yasc\n" + ERRORS[name])
    assert proc.returncode != 0
    assert GONE in proc.stderr, proc.stderr


NAMES = {
    "variable": "int readonly = 3;\nint f(void) { return readonly; }\n",
    "pointer_named": "int g(const char * view s) { const char * view readonly = s; return *readonly; }\n",
    "plain_named": "char *readonly;\n",
    "array_named": "const char * view readonly[2];\n",
    "times": "int f(int x, int readonly) { return x * readonly; }\n",
}


@pytest.mark.parametrize("name", sorted(NAMES))
def test_a_name_in_yasc(tmp_path, name):
    proc = _compile(tmp_path, "#pragma yasc\n" + NAMES[name])
    assert GONE not in proc.stderr, proc.stderr
    assert proc.returncode == 0 or name == "plain_named", proc.stderr


def test_a_name_in_regular_c(tmp_path):
    proc = _compile(tmp_path, "int readonly = 3;\nint f(void) { int *readonly2 = &readonly; return *readonly2; }\n",
                    "-ccompat")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_the_long_spelling(tmp_path):
    """what it said: `const T * view`, and `const` at each level that has it"""
    src = ("#pragma yasc\n"
           "int main(int argc, const char * view const * view argv) { return argc > 1 && argv[1][0]; }\n"
           "int n(const char * view s) { const char * view p = s; return *p; }\n")
    proc = _compile(tmp_path, src, "-Werror")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
