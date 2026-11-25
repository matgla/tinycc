"""Safe by default, regular C with -ccompat.

Without -ccompat (or an explicit -std=c.. / gnu..), tcc compiles the yasc
dialect with -Wall and -Wlifetime, and unsafe C is an error.  -ccompat is the
old behaviour: those are warnings, the yasc words are identifiers.  An option
the command line sets keeps what it says, wherever it comes.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

UNSAFE = [
    ("int f(void) { return undeclared(); }", "implicit-function-declaration",
     "implicit declaration of function 'undeclared'"),
    ("int f(int * VIEW p) { int x = p; return x; }", "int-conversion",
     "assignment makes integer from pointer without a cast"),
    ("int * VIEW f(int * VIEW s, int x) { int * VIEW p = x; return p; }", "int-conversion",
     "assignment makes pointer from integer without a cast"),
    ("int f(int * VIEW p, long q) { return p == q; }", "int-conversion",
     "pointer/integer mismatch in comparison"),
    ("long * VIEW f(int * VIEW p) { long * VIEW q = p; return q; }", "incompatible-pointer-types",
     "assignment from incompatible pointer type"),
    ("int f(int x) { if (x) return 1; }", "return-type", "function might return no value: 'f'"),
    ("int f(void) { return; }", "return-type", "'return' with no value"),
    ("void f(int x) { return x; }", "return-type", "void function returns a value"),
    ("x;", "implicit-int", "type defaults to int"),
    ("char * VIEW f(const char * VIEW s) { char * VIEW t = s; return t; }", "discarded-qualifiers",
     "discards 'const' qualifier from pointer target type"),
]


# The default is yasc, where every pointer says owned or view; -ccompat is
# regular C, where `view` is a name.  VIEW is the one or the other.
VIEW = "#ifdef __YASC__\n#define VIEW view\n#else\n#define VIEW\n#endif\n"


def _said(stderr, kind, message):
    """A `kind: ...` line carrying message: a message that names types (`char
    * view` in yasc, `char *` in regular C) is matched by its mode-free part."""
    return any(f"{kind}: " in line and message in line for line in stderr.splitlines())


def _compile(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(VIEW + body + "\n")
    return subprocess.run(
        [str(TCC), "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True, text=True,
    )


@pytest.mark.parametrize("body, flag, message", UNSAFE, ids=[u[1] + str(i) for i, u in enumerate(UNSAFE)])
def test_unsafe_c_is_an_error_by_default(tmp_path, body, flag, message):
    proc = _compile(tmp_path, body)
    assert proc.returncode != 0 and _said(proc.stderr, "error", message), proc.stderr


@pytest.mark.parametrize("compat", [("-ccompat",), ("-std=c11",), ("-std=gnu17",), ("-std=c23",)])
@pytest.mark.parametrize("body, flag, message", UNSAFE, ids=[u[1] + str(i) for i, u in enumerate(UNSAFE)])
def test_regular_c_only_warns(tmp_path, body, flag, message, compat):
    proc = _compile(tmp_path, body, *compat)
    assert proc.returncode == 0 and _said(proc.stderr, "warning", message), proc.stderr


@pytest.mark.parametrize("body, flag, message", UNSAFE, ids=[u[1] + str(i) for i, u in enumerate(UNSAFE)])
def test_wno_error_wins_in_any_position(tmp_path, body, flag, message):
    for flags in ((f"-Wno-error={flag}",), (f"-Wno-error={flag}", "-O1"), ("-O1", f"-Wno-error={flag}")):
        proc = _compile(tmp_path, body, *flags)
        assert proc.returncode == 0 and _said(proc.stderr, "warning", message), proc.stderr


def test_ccompat_anywhere_wins(tmp_path):
    body = UNSAFE[0][0]
    for flags in (("-ccompat", "-O1"), ("-O1", "-ccompat"), ("-Wall", "-ccompat")):
        assert _compile(tmp_path, body, *flags).returncode == 0


def test_wall_is_on_by_default(tmp_path):
    body = "int f(void) { __label__ unused; return 0; }"
    assert "label 'unused' declared but not used" in _compile(tmp_path, body).stderr
    assert "unused" not in _compile(tmp_path, body, "-ccompat").stderr


def test_yasc_is_on_by_default(tmp_path):
    body = "#ifndef __YASC__\n#error no yasc\n#endif\nchar * owned f(void);"
    assert _compile(tmp_path, body).returncode == 0
    assert _compile(tmp_path, body, "-ccompat").returncode != 0
    proc = _compile(tmp_path, "int out;")
    assert proc.returncode != 0 and "'out' is a yasc keyword: rename it, or compile with -ccompat" in proc.stderr
    assert _compile(tmp_path, "int out;", "-ccompat").returncode == 0


def test_yasc_keyword_used_as_a_name(tmp_path):
    # after a '*' they qualify the pointer (`f(char * owned)` is a parameter
    # with no name), and after an int a handle (`f(int owned)` too), so the
    # misuse is a name where one is expected
    for body, kw in [("int view = 1;", "view"), ("int f(int out);", "out"), ("int owned;", "owned"),
                     ("struct S { int drop; };", "drop"), ("int maybe, x;", "maybe")]:
        proc = _compile(tmp_path, body)
        assert f"'{kw}' is a yasc keyword" in proc.stderr, (body, proc.stderr)


def test_safe_code_compiles_cleanly(tmp_path):
    body = """\
void * owned maybe alloc(unsigned n);
void release(drop void * owned maybe p);
int use(void);
static int sum(const int * view v, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += v[i];
  return s;
}
int use(void)
{
  int * owned maybe v = alloc(4 * sizeof(int));
  int r = 0;
  if (v)
    r = sum(v, 0);
  release(v);
  return r;
}"""
    proc = _compile(tmp_path, body)
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr


def test_pragma_yasc_is_c23_under_ccompat(tmp_path):
    """yasc is C23: a regular-C build (-ccompat, C11) of a file that says
    #pragma yasc has nullptr, true and false there (libc's libgen.c)"""
    src = tmp_path / "p.c"
    src.write_text("#pragma yasc\nint t0(int * view maybe q)\n{\n  return q == nullptr;\n}\n"
                   "bool t(void)\n{\n  return true && !false;\n}\n")
    proc = subprocess.run([str(TCC), "-ccompat", "-Wall", "-Werror", "-c", str(src), "-o", str(tmp_path / "p.o")],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
