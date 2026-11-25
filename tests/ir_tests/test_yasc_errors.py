"""yasc error unions: what the compiler rejects, and what stays plain C.

The run-time behaviour is in yasc_errors.c (test_qemu.py, every -O level).
An error is a non-zero int (errno values, or a hash of the name); `!T` is a
T or an error, and can only be used through try, catch, return, assignment
of the same type or an argument of that type.  docs/plans/yasc_errors.md.
"""

import re
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
#define ENOENT 2
#define EACCES 13
error ParseError { BadDigit, Overflow, Empty };
error FsError { FileNotFound = ENOENT, EACCES, BadPath };
!int f(void);
!void g(void);
void note(int);
"""

MISUSE = "an error union needs try or catch here"
DISCARD = "an error union is discarded: use try or catch"


def _compile(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(body)
    return subprocess.run(
        [str(TCC), "-ccompat", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _yasc(tmp_path, body, *flags):
    return _compile(tmp_path, PRELUDE + body, *flags)


def _ok(proc):
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def _error(proc):
    assert proc.returncode != 0, "expected a compile error"
    lines = [line.split("error: ", 1)[1] for line in proc.stderr.splitlines() if "error: " in line]
    assert lines, proc.stderr
    return lines[0]


def test_keywords_are_identifiers_outside_yasc(tmp_path):
    body = "int try = 1, catch = 2, errdefer = 3, error_from = 4;\nint error(int x) { return x + try + catch; }\n"
    _ok(_compile(tmp_path, body, "-std=c23"))


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_correct_code_is_silent(tmp_path, opt):
    body = """\
!char * borrows(static) name(int i);
!int (*hook)(void) = f;
static int error_count;
!int h(int a)
{
  errdefer error_count++;
  defer note(a);
  int v = try f();
  char * view p = name(v) catch 0;
  try g();
  if (!p)
    return error.Empty;
  if (a < 0)
    return error.ENOENT;
  error ParseError pe = error.Overflow;
  if (a > 100)
    return pe;
  error any = pe;
  error FsError fe = error.EACCES;
  int code = fe;
  return v + code + (any == fe) + (hook() catch 0) + (f() catch |e| e);
}
"""
    _ok(_yasc(tmp_path, body, opt))


def test_error_as_a_variable_name(tmp_path):
    _ok(_yasc(tmp_path, "int k(void) { int error = 3; return error + 1; }\n"))


def test_discarded(tmp_path):
    assert _error(_yasc(tmp_path, "void k(void) { f(); }\n")) == DISCARD


def test_discarded_by_void_cast(tmp_path):
    assert _error(_yasc(tmp_path, "void k(void) { (void)f(); }\n")) == DISCARD


def test_discarded_void_union(tmp_path):
    assert _error(_yasc(tmp_path, "void k(void) { g(); }\n")) == DISCARD


@pytest.mark.parametrize(
    "body",
    [
        "int k(void) { return f() + 1; }\n",
        "int k(void) { if (f()) return 1; return 0; }\n",
        "int k(void) { int x = f(); return x; }\n",
        "int k(void) { return !f(); }\n",
        "unsigned long long k(void) { return f(); }\n",
        "void k(void) { note(f()); }\n",
    ],
)
def test_used_without_try_or_catch(tmp_path, body):
    assert _error(_yasc(tmp_path, body)) == MISUSE


def test_union_payloads_must_match(tmp_path):
    proc = _yasc(tmp_path, "!char * borrows(static) p(void);\n!int k(void) { return p(); }\n")
    assert _error(proc) == "cannot convert '!char * view' to '!int'"


def test_try_outside_an_error_union_function(tmp_path):
    proc = _yasc(tmp_path, "int k(void) { return try f(); }\n")
    assert _error(proc) == "try in a function that does not return an error union"


def test_errdefer_outside_an_error_union_function(tmp_path):
    proc = _yasc(tmp_path, "int k(void) { errdefer note(1); return 0; }\n")
    assert _error(proc) == "errdefer in a function that does not return an error union"


def test_try_in_a_defer_body(tmp_path):
    proc = _yasc(tmp_path, "!int k(void) { defer { try g(); } return 0; }\n")
    assert _error(proc) == "try in a defer body"


def test_try_needs_an_error_union(tmp_path):
    proc = _yasc(tmp_path, "!int k(int a) { return try a; }\n")
    assert _error(proc) == "try needs an error union"


def test_catch_needs_an_error_union(tmp_path):
    proc = _yasc(tmp_path, "int k(int a) { return a catch 0; }\n")
    assert _error(proc) == "catch needs an error union"


def test_return_without_value(tmp_path):
    proc = _yasc(tmp_path, "!int k(void) { return; }\n")
    assert _error(proc) == "'return' with no value"


def test_void_union_returns_a_value(tmp_path):
    proc = _yasc(tmp_path, "!void k(void) { return 1; }\n")
    assert _error(proc) == "a !void function returns a value"


def test_errno_name_needs_errno_h(tmp_path):
    proc = _compile(tmp_path, "#pragma yasc\n!int k(void) { return error.EPERM; }\n")
    assert _error(proc) == "error.EPERM needs <errno.h>"


def test_int_is_not_an_error(tmp_path):
    proc = _yasc(tmp_path, "!int k(int a) { error FsError e = a; return e; }\n")
    assert _error(proc) == "an int is not an error: use error_from()"


def test_constant_not_a_member(tmp_path):
    proc = _yasc(tmp_path, "!int k(void) { error ParseError p = error.ENOENT; return p; }\n")
    assert _error(proc) == "error.ENOENT is not a member of error ParseError"


def test_set_does_not_convert(tmp_path):
    proc = _yasc(tmp_path, "!int k(error ParseError p) { error FsError e = p; return e; }\n")
    assert _error(proc) == "error ParseError does not convert to error FsError"


def test_any_error_does_not_narrow(tmp_path):
    proc = _yasc(tmp_path, "!int k(error any) { error FsError e = any; return e; }\n")
    assert _error(proc) == "error does not convert to error FsError"


def test_catch_block_that_completes_has_no_value(tmp_path):
    proc = _yasc(tmp_path, "int k(void) { int x = f() catch |e| { note(e); }; return x; }\n")
    assert "void" in _error(proc)


def test_unsupported_payload(tmp_path):
    proc = _yasc(tmp_path, "!double d(void);\n")
    assert _error(proc) == (
        "!double: an error union holds void, an integer of at most 32 bits, a pointer, an enum or an error set"
    )


def test_redefined_set(tmp_path):
    proc = _yasc(tmp_path, "error ParseError { Again };\n")
    assert _error(proc) == "redefinition of error ParseError"


def test_union_is_not_unsigned_long_long(tmp_path):
    body = "#pragma yasc\n!int f(void);\nunsigned long long f(void);\n"
    proc = _compile(tmp_path, body)
    assert proc.returncode != 0


@pytest.mark.parametrize("decl", ["static !int", "static !void", "static inline !int"])
def test_storage_class_before_the_union_stays(tmp_path, decl):
    # `static` before the '!' used to be dropped: the function was global
    # (and -Wmissing-prototypes asked for a prototype of it)
    ret = "return;" if "void" in decl else "return 1;"
    proc = _yasc(tmp_path, f"{decl} s(void) {{ {ret} }}\nvoid use(void);\nvoid use(void) {{ s() catch {{}}; }}\n",
                 "-Wmissing-prototypes", "-Wno-unused-function")
    assert "no previous prototype for 's'" not in proc.stderr, proc.stderr
    assert proc.returncode == 0, proc.stderr
    nm = subprocess.run(["arm-none-eabi-nm", str(tmp_path / "t.o")], capture_output=True, text=True).stdout
    assert not re.search(r"(?m) T s$", nm), nm
