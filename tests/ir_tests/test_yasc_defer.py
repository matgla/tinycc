"""yasc `defer`: what the compiler rejects, and what stays plain C.

The run-time behaviour is in yasc_defer.c (test_qemu.py, every -O level).
A defer body is replayed at each exit of its scope, so it may not leave the
body itself (return, break or continue out of it, goto, labels), and a defer
directly in a switch body would run for cases that jumped past it.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = "#pragma yasc\nvoid note(int);\n"


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


def _error(proc):
    assert proc.returncode != 0, "expected a compile error"
    lines = [line.split("error: ", 1)[1] for line in proc.stderr.splitlines() if "error: " in line]
    assert lines, proc.stderr
    return lines[0]


def test_defer_is_an_identifier_outside_yasc(tmp_path):
    proc = _compile(tmp_path, "int defer = 1;\nint f(int errdefer) { return defer + errdefer; }\n", "-std=c23")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_defer_compiles(tmp_path, opt):
    proc = _yasc(tmp_path, "int f(int a) { defer note(a); if (a) return 1; defer { note(2); } return 0; }\n", opt)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_return_in_defer(tmp_path):
    proc = _yasc(tmp_path, "int f(void) { defer { return 1; } return 0; }\n")
    assert _error(proc) == "'return' in a defer body"


def test_break_out_of_defer(tmp_path):
    proc = _yasc(tmp_path, "void f(void) { for (;;) { defer break; note(1); } }\n")
    assert _error(proc) == "'break' out of a defer body"


def test_continue_out_of_defer(tmp_path):
    proc = _yasc(tmp_path, "void f(void) { while (1) { defer { continue; } note(1); } }\n")
    assert _error(proc) == "'continue' out of a defer body"


def test_break_inside_a_loop_of_the_body(tmp_path):
    proc = _yasc(tmp_path, "void f(void) { defer { for (int i = 0; i < 3; i++) { if (i) break; note(i); } } }\n")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_goto_in_defer(tmp_path):
    proc = _yasc(tmp_path, "void f(void) { defer { goto done; } note(1); done: ; }\n")
    assert _error(proc) == "'goto' in a defer body"


def test_label_in_defer(tmp_path):
    proc = _yasc(tmp_path, "void f(void) { defer { here: note(1); } }\n")
    assert _error(proc) == "label in a defer body"


def test_defer_directly_in_switch(tmp_path):
    proc = _yasc(tmp_path, "void f(int v) { switch (v) { case 1: defer note(1); case 2: note(2); } }\n")
    assert _error(proc) == "defer directly in a switch body: put the case in braces"


def test_defer_in_braced_case(tmp_path):
    proc = _yasc(tmp_path, "void f(int v) { switch (v) { case 1: { defer note(1); break; } case 2: note(2); } }\n")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_body_cannot_see_later_names(tmp_path):
    proc = _yasc(tmp_path, "void f(void) { defer note(later); int later = 1; note(later); }\n")
    assert _error(proc) == "'later' undeclared"


def test_warning_once_for_many_exits(tmp_path):
    body = "void f(int a) { defer { int *view p = 1; note(*p); } if (a) return; if (a > 1) return; note(a); }\n"
    proc = _yasc(tmp_path, body, "-Wall")
    assert proc.returncode == 0, proc.stderr
    warnings = [line for line in proc.stderr.splitlines() if "warning: " in line]
    assert len(warnings) == 1, proc.stderr


def test_inlined_function_with_return_in_a_defer_body(tmp_path):
    body = "static inline int g(int a) { if (a) return 1; return 2; }\nvoid f(int a) { defer note(g(a)); note(0); }\n"
    for opt in ("-O0", "-O2"):
        proc = _yasc(tmp_path, body, opt)
        assert proc.returncode == 0 and not proc.stderr, proc.stderr
