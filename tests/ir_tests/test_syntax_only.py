"""-fsyntax-only: parse and check, generate and write nothing.

Each function gets the check-only parse (gen_function_check_only): every
frontend diagnostic and the flow checks (-Wlifetime, -W[maybe-]uninitialized,
-Wreturn-local-addr), no optimisation, no code, no object, no link.  Errors
still fail the compile.  For CI sweeps of the whole tree.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")


def _run(tmp_path, files, *flags):
    for name, body in files.items():
        (tmp_path / name).write_text(body)
    return subprocess.run(
        [str(TCC), "-fsyntax-only", *flags, *files], capture_output=True, text=True, cwd=tmp_path
    )


def test_writes_nothing(tmp_path):
    proc = _run(tmp_path, {"a.c": "int main(void) { return 0; }\n"}, "-ccompat")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    assert sorted(p.name for p in tmp_path.iterdir()) == ["a.c"]


def test_ignores_o(tmp_path):
    proc = _run(tmp_path, {"a.c": "int x;\n"}, "-ccompat", "-o", "out.o")
    assert proc.returncode == 0 and not (tmp_path / "out.o").exists()


def test_errors_fail(tmp_path):
    proc = _run(tmp_path, {"a.c": "int f(void) { return 0 }\n"}, "-ccompat")
    assert proc.returncode != 0 and "';' expected" in proc.stderr


def test_safe_default_errors(tmp_path):
    proc = _run(tmp_path, {"a.c": "int f(void) { return undeclared(); }\n"})
    assert proc.returncode != 0 and "implicit declaration of function 'undeclared'" in proc.stderr


def test_every_file(tmp_path):
    files = {"a.c": "int a(void) { int x; return x; }\n", "b.c": "int b(void) { int y; return y; }\n"}
    proc = _run(tmp_path, files, "-ccompat", "-Wuninitialized")
    assert "a.c:1: warning: 'x' is used uninitialized" in proc.stderr
    assert "b.c:1: warning: 'y' is used uninitialized" in proc.stderr
    assert sorted(p.name for p in tmp_path.iterdir()) == ["a.c", "b.c"]


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_flow_checks(tmp_path, opt):
    body = """\
#pragma yasc
void free(drop void * owned maybe p);
static void twice(char * owned p) { free(p); free(p); }
void caller(char * owned p) { twice(p); }
int * borrows(static) local(void) { int x = 1; return &x; }
"""
    proc = _run(tmp_path, {"a.c": body}, "-ccompat", "-Wlifetime", "-Wall", opt)
    assert proc.returncode == 0, proc.stderr
    assert "a.c:3: warning: 'p' is freed twice" in proc.stderr
    assert "a.c:5: warning: 'local' returns the address of a local variable" in proc.stderr


def test_same_diagnostics_as_a_compile(tmp_path):
    body = "int g(int);\nint f(int a) { int x; if (a) x = g(a); return x + h(a); }\n"
    (tmp_path / "a.c").write_text(body)
    full = subprocess.run(
        [str(TCC), "-ccompat", "-Wall", "-Wmaybe-uninitialized", "-c", "a.c", "-o", "a.o"],
        capture_output=True, text=True, cwd=tmp_path,
    )
    syn = subprocess.run(
        [str(TCC), "-ccompat", "-Wall", "-Wmaybe-uninitialized", "-fsyntax-only", "a.c"],
        capture_output=True, text=True, cwd=tmp_path,
    )
    assert full.stderr == syn.stderr and "may be used uninitialized" in syn.stderr
