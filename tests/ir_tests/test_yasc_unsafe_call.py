"""-Wyasc-unsafe-call: a `yasc_unsafe` function called by yasc code outside unsafe.

yasos libc marks strcpy, stpcpy, strcat, strncat, sprintf and vsprintf (their
destination has no size): yasc code calls them inside unsafe { } or
unsafe(...) only, else the warning says why and the note names the bounded
call -- strlcpy(b, src, sizeof b), snprintf(b, sizeof b, ...) -- when the
destination's size is known.  A C section and regular C are not told.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

DECLS = """\
char *borrows(d) strcpy(out char *d, char *s) __attribute__((yasc_unsafe("the destination has no size")));
#pragma tcc c_begin
int sprintf(char *d, const char *f, ...) __attribute__((__yasc_unsafe__));
#pragma tcc c_end
void use(char *p);
"""

WARN_CPY = "'strcpy' is unsafe: the destination has no size (inside unsafe { } it is allowed)"
WARN_PRINTF = "'sprintf' is unsafe: nothing bounds what it writes (inside unsafe { } it is allowed)"


def _run(tmp_path, body, *flags, yasc=True):
    src = tmp_path / "t.c"
    src.write_text(("#pragma yasc\n" if yasc else "") + DECLS + body)
    return subprocess.run([str(TCC), "-fsyntax-only", "-Wno-missing-prototypes", *flags, str(src)],
                          capture_output=True, text=True)


def test_outside_unsafe_warns_with_the_bounded_call(tmp_path):
    proc = _run(tmp_path, "void f(char *s, int x)\n{\n  char b[8] = undefined;\n  strcpy(b, s);\n"
                          "  sprintf(b, \"%d\", x);\n  use(b);\n}\n")
    assert proc.returncode == 0, proc.stderr
    assert WARN_CPY in proc.stderr, proc.stderr
    assert "note: use strlcpy(b, src, sizeof b), which bounds the copy" in proc.stderr, proc.stderr
    assert WARN_PRINTF in proc.stderr, proc.stderr
    assert "note: use snprintf(b, sizeof b, ...), which bounds the output" in proc.stderr, proc.stderr
    assert "declared here" in proc.stderr, proc.stderr


@pytest.mark.parametrize("body", [
    "void f(char *s)\n{\n  char b[8] = undefined;\n  unsafe {\n    strcpy(b, s);\n  }\n  use(b);\n}\n",
    "void f(char *s)\n{\n  char b[8] = undefined;\n  unsafe(strcpy(b, s));\n  use(b);\n}\n",
    "#pragma tcc c_begin\nvoid f(char *b, const char *s) { strcpy(b, s); }\n#pragma tcc c_end\n",
])
def test_allowed_inside_unsafe_and_in_c(tmp_path, body):
    proc = _run(tmp_path, body)
    assert proc.returncode == 0, proc.stderr
    assert "is unsafe" not in proc.stderr, proc.stderr


def test_regular_c_is_not_told(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("char *strcpy(char *d, const char *s) __attribute__((yasc_unsafe(\"x\")));\n"
                   "void f(char *b, const char *s) { strcpy(b, s); }\n")
    proc = subprocess.run([str(TCC), "-ccompat", "-Wall", "-fsyntax-only", str(src)], capture_output=True, text=True)
    assert proc.returncode == 0 and "unsafe" not in proc.stderr, proc.stderr


def test_switch_off(tmp_path):
    proc = _run(tmp_path, "void f(char *s)\n{\n  char b[8] = undefined;\n  strcpy(b, s);\n  use(b);\n}\n",
                "-Wno-yasc-unsafe-call")
    assert proc.returncode == 0 and "unsafe" not in proc.stderr, proc.stderr


def test_has_attribute(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("#if !__has_attribute(__yasc_unsafe__)\n#error no\n#endif\n")
    proc = subprocess.run([str(TCC), "-ccompat", "-E", str(src)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
