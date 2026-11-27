"""tcc -E writes gcc's line markers, `# N "file" flags`: 1 entering a header,
2 returning to the file that included it, 3 for a system header's text.
#pragma GCC system_header is done by -E and its line goes (as with gcc): a
marker with 3 follows.  Compiling the output reads the flags back: a system
header's text is a system header again (its warnings quiet, -Wsystem-headers
gives them), and a file is in the yasc dialect it had when a marker returns
to it -- a header starts as regular C, its #pragma yasc is in the output.
Before, the compile of -E output had no system headers and read every
header in the main file's dialect.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")


def _tcc(cwd, *args):
    return subprocess.run([str(TCC), *args], capture_output=True, text=True, cwd=cwd)


def _markers(text):
    return [line for line in text.splitlines() if line.startswith("# ")]


def _places(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            out.append(parts[0].rsplit("/", 1)[-1])
    return out


def _setup(tmp_path):
    (tmp_path / "inc").mkdir()
    (tmp_path / "h.h").write_text('int a;\n#pragma GCC system_header\nint *b(void) { return 1; }\n#include "h2.h"\nint c;\n')
    (tmp_path / "h2.h").write_text("int *d(void) { return 2; }\n")
    (tmp_path / "inc" / "s.h").write_text("int *e(void) { return 3; }\n")
    (tmp_path / "m.c").write_text('#include "h.h"\n#include <s.h>\nint *m(void) { return 4; }\n')


def test_flags_written(tmp_path):
    _setup(tmp_path)
    proc = _tcc(tmp_path, "-ccompat", "-isystem", "inc", "-E", "m.c")
    assert proc.returncode == 0, proc.stderr
    assert "system_header" not in proc.stdout
    assert _markers(proc.stdout)[3:] == [
        '# 1 "h.h" 1',
        '# 3 "h.h" 3',
        '# 1 "h2.h" 1 3',
        '# 5 "h.h" 2 3',
        '# 2 "m.c" 2',
        '# 1 "inc/s.h" 1 3',
        '# 3 "m.c" 2',
    ], proc.stdout


def test_line_directives_without_flags(tmp_path):
    """-P1 writes #line, which has no flags; -P none at all."""
    _setup(tmp_path)
    proc = _tcc(tmp_path, "-ccompat", "-isystem", "inc", "-E", "-P1", "m.c")
    assert proc.returncode == 0, proc.stderr
    lines = [line for line in proc.stdout.splitlines() if line.startswith("#")]
    assert '#line 1 "h2.h"' in lines and all(line.startswith("#line ") and line.endswith('"') for line in lines), lines
    proc = _tcc(tmp_path, "-ccompat", "-isystem", "inc", "-E", "-P", "m.c")
    assert "#" not in proc.stdout, proc.stdout


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_output_compiles_with_system_headers(tmp_path, opt):
    _setup(tmp_path)
    direct = _tcc(tmp_path, "-ccompat", "-Wall", "-isystem", "inc", opt, "-c", "m.c", "-o", "a.o")
    pre = _tcc(tmp_path, "-ccompat", "-isystem", "inc", "-E", "m.c", "-o", "m.i")
    assert pre.returncode == 0, pre.stderr
    proc = _tcc(tmp_path, "-ccompat", "-Wall", opt, "-c", "m.i", "-o", "b.o")
    assert proc.returncode == 0, proc.stderr
    # only the main file's warning, as compiling m.c directly
    assert _places(proc) == _places(direct) == ["m.c:3"], proc.stderr
    proc = _tcc(tmp_path, "-ccompat", "-Wall", "-Wsystem-headers", opt, "-c", "m.i", "-o", "b.o")
    assert sorted(_places(proc)) == ["h.h:3", "h2.h:1", "m.c:3", "s.h:1"], proc.stderr


def test_lines_before_the_pragma_are_not_system(tmp_path):
    (tmp_path / "h.h").write_text("int *a(void) { return 1; }\n#pragma GCC system_header\nint *b(void) { return 2; }\n")
    (tmp_path / "m.c").write_text('#include "h.h"\n')
    assert _tcc(tmp_path, "-ccompat", "-E", "m.c", "-o", "m.i").returncode == 0
    proc = _tcc(tmp_path, "-ccompat", "-Wall", "-c", "m.i", "-o", "m.o")
    assert _places(proc) == ["h.h:1"], proc.stderr


def test_safe_default_errors_quiet_in_output(tmp_path):
    """The safe default's int-conversion error in a system header: quiet
    compiling the -E output too."""
    _setup(tmp_path)
    (tmp_path / "m.c").write_text('#include "h.h"\n#include <s.h>\n')
    assert _tcc(tmp_path, "-isystem", "inc", "-E", "m.c", "-o", "m.i").returncode == 0
    proc = _tcc(tmp_path, "-c", "m.i", "-o", "m.o")
    assert proc.returncode == 0, proc.stderr


def test_written_markers_read_by_hand(tmp_path):
    """A hand-written marker with 3, as gcc's -E output has."""
    (tmp_path / "m.i").write_text(
        '# 1 "m.c"\n# 1 "/usr/include/x.h" 1 3\nint *x(void) { return 1; }\n# 2 "m.c" 2\nint *y(void) { return 2; }\n'
    )
    proc = _tcc(tmp_path, "-ccompat", "-Wall", "-c", "m.i", "-o", "m.o")
    assert proc.returncode == 0, proc.stderr
    assert _places(proc) == ["m.c:2"], proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_yasc_dialect_through_output(tmp_path, opt):
    """-std=yasc: a regular-C header stays regular C in the output, a
    #pragma yasc header is yasc, and the main file is yasc again after both;
    a static inline from the main file replayed at -O2 too."""
    (tmp_path / "c.h").write_text("int view(int owned);\nstatic inline int helper(int maybe) { int out = maybe; return out; }\n")
    (tmp_path / "y.h").write_text("#pragma yasc\nchar *owned yalloc(void);\n")
    (tmp_path / "m.c").write_text(
        '#include "c.h"\n#include "y.h"\n'
        "static inline int k(char *p) { return p[0]; }\n"
        "int f(char *p) { return view(helper(1)) + k(p) + k(p); }\n"
    )
    assert _tcc(tmp_path, "-std=yasc", "-E", "m.c", "-o", "m.i").returncode == 0
    a = _tcc(tmp_path, "-std=yasc", opt, "-c", "m.c", "-o", "a.o")
    b = _tcc(tmp_path, "-std=yasc", opt, "-c", "m.i", "-o", "b.o")
    assert a.returncode == 0, a.stderr
    assert b.returncode == 0, b.stderr
    # the main file is yasc again after the headers (a function's plain
    # result says so: a parameter's or a global's own pointer is a view)
    (tmp_path / "m.c").write_text('#include "c.h"\nchar *gp(void);\n')
    assert _tcc(tmp_path, "-std=yasc", "-E", "m.c", "-o", "m.i").returncode == 0
    b = _tcc(tmp_path, "-std=yasc", opt, "-c", "m.i", "-o", "b.o")
    assert b.returncode != 0 and "plain pointer in yasc" in b.stderr, b.stderr
