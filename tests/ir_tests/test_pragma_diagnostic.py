"""#pragma GCC|tcc|clang diagnostic push / pop / ignored / warning / error.

`ignored "-Wname"` silences a warning, `warning` makes it a warning even under
-Werror, `error` an error; push/pop save and restore the set.  _Pragma works
too, and tcc -E keeps the lines.  A diagnostic reported after its line was
parsed -- the flow checks run at the end of a function, by when a `pop`
right after the closing brace has been read -- sees the options as they were
at its line.  The next unit starts from the command line's options.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _run(tmp_path, body, *flags, name="t.c"):
    src = tmp_path / name
    src.write_text(body)
    return subprocess.run(
        [str(TCC), "-ccompat", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")], capture_output=True, text=True
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error", "note"):
            out.append((int(parts[0].rsplit(":", 1)[1]), parts[1], parts[2]))
    return out


UNINIT = """\
int a(void) { int x; return x; }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
int b(void) { int x; return x; }
#pragma GCC diagnostic pop
int c(void) { int x; return x; }
"""


@pytest.mark.parametrize("opt", LEVELS)
def test_push_ignored_pop(tmp_path, opt):
    proc = _run(tmp_path, UNINIT, "-Wuninitialized", opt)
    assert [d[0] for d in _diags(proc)] == [1, 6], proc.stderr


@pytest.mark.parametrize("spelling", ["GCC", "tcc", "clang"])
def test_spellings(tmp_path, spelling):
    proc = _run(tmp_path, UNINIT.replace("GCC", spelling), "-Wuninitialized")
    assert [d[0] for d in _diags(proc)] == [1, 6], proc.stderr


def test_error_and_warning(tmp_path):
    body = (
        '#pragma GCC diagnostic error "-Wuninitialized"\nint a(void) { int x; return x; }\n'
        '#pragma GCC diagnostic warning "-Wuninitialized"\nint b(void) { int x; return x; }\n'
    )
    proc = _run(tmp_path, body, "-Wuninitialized")
    assert proc.returncode != 0
    assert [d[:2] for d in _diags(proc)] == [(2, "error"), (4, "warning")]


def test_warning_beats_werror(tmp_path):
    body = '#pragma GCC diagnostic warning "-Wuninitialized"\nint a(void) { int x; return x; }\n'
    proc = _run(tmp_path, body, "-Wuninitialized", "-Werror")
    assert proc.returncode == 0 and [d[1] for d in _diags(proc)] == ["warning"], proc.stderr


def test_switches_a_warning_on(tmp_path):
    body = '#pragma GCC diagnostic warning "-Wuninitialized"\nint a(void) { int x; return x; }\n'
    proc = _run(tmp_path, body)
    assert [d[:2] for d in _diags(proc)] == [(2, "warning")], proc.stderr


def test_frontend_warning(tmp_path):
    body = (
        '#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"\nint f(void) { return g1(); }\n'
        '#pragma GCC diagnostic warning "-Wimplicit-function-declaration"\nint h(void) { return g2(); }\n'
    )
    proc = _run(tmp_path, body, "-Wall")
    assert [d[0] for d in _diags(proc)] == [4], proc.stderr


def test_wall(tmp_path):
    body = '#pragma GCC diagnostic ignored "-Wall"\nint f(void) { return g1(); }\n'
    proc = _run(tmp_path, body, "-Wall")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_pragma_operator(tmp_path):
    body = (
        '#define QUIET _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \\"-Wuninitialized\\"")\n'
        '#define LOUD _Pragma("GCC diagnostic pop")\n'
        "QUIET\nint a(void) { int x; return x; }\nLOUD\nint b(void) { int x; return x; }\n"
    )
    proc = _run(tmp_path, body, "-Wuninitialized")
    assert [d[0] for d in _diags(proc)] == [6], proc.stderr


LIFETIME = """\
#pragma yasc
void free(drop void * owned maybe p);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wlifetime"
void quiet(char * owned p) { free(p); free(p); }
#pragma GCC diagnostic pop
void loud(char * owned p) { free(p); free(p); }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wlifetime"
static void quiet_static(char * owned p) { free(p); free(p); }
#pragma GCC diagnostic pop
void caller(char * owned p) { quiet_static(move(p)); }
"""


@pytest.mark.parametrize("opt", LEVELS)
def test_flow_check_sees_its_lines_options(tmp_path, opt):
    """quiet's report comes at its end, after the pop was read; quiet_static
    is compiled at the end of the unit at -O1+."""
    proc = _run(tmp_path, LIFETIME, "-Wlifetime", opt)
    assert _diags(proc) == [(7, "warning", "'p' is freed twice"), (7, "note", "'p' was freed here")], proc.stderr


def test_unknown_option_and_unbalanced_pop(tmp_path):
    body = '#pragma GCC diagnostic ignored "-Wbogus"\n#pragma GCC diagnostic pop\nint x;\n'
    proc = _run(tmp_path, body)
    assert proc.returncode == 0 and "unknown warning option" not in proc.stderr
    assert "pop without a push" in proc.stderr
    proc = _run(tmp_path, body, "-Wunsupported")
    assert "unknown warning option '-Wbogus'" in proc.stderr


def test_preprocess_keeps_the_lines(tmp_path):
    src = tmp_path / "t.c"
    src.write_text(UNINIT)
    proc = subprocess.run([str(TCC), "-ccompat", "-E", str(src)], capture_output=True, text=True)
    assert proc.stdout.count("#pragma GCC diagnostic") == 3, proc.stdout


def test_next_unit_starts_clean(tmp_path):
    (tmp_path / "a.c").write_text('#pragma GCC diagnostic ignored "-Wuninitialized"\nint a(void) { return 0; }\n')
    (tmp_path / "b.c").write_text("int b(void) { int x; return x; }\n")
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-Wuninitialized", "-c", str(tmp_path / "a.c"), str(tmp_path / "b.c")],
        capture_output=True,
        text=True,
        cwd=tmp_path,
    )
    assert "'x' is used uninitialized" in proc.stderr, proc.stderr
