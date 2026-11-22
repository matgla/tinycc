"""#pragma GCC system_header: the rest of the header is a system header, as one
found on a system include path -- its warnings are silenced (the checks that
honour system headers), and a "..." include next to it is one too.  The
lines before the pragma are not.  It is honoured in a body replayed after its
file was read: an inline expansion (-O1+), a deferred function, an inline
function generated at the end of the unit.  In the main file it is ignored,
as gcc does.  Before, -Wall reported "#pragma GCC system_header ignored" for
every header that says it.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _run(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(body)
    return subprocess.run(
        [str(TCC), "-ccompat", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")], capture_output=True, text=True
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] == "warning":
            out.append((parts[0].rsplit("/", 1)[-1], parts[2]))
    return out


ARITH = "pointer of type 'void *' used in arithmetic"

HEADER = """\
static inline int before(void *p) { return p + 1 != 0; }
#pragma GCC system_header
static inline int hf(void *p) { return p + 1 != 0; }
static int hs(void *p) { return p + 2 != 0; }
int hx(void *p) { return p + 3 != 0; }
int old();
#include "near.h"
"""


@pytest.mark.parametrize("opt", LEVELS)
def test_silenced_from_the_pragma_on(tmp_path, opt):
    (tmp_path / "h.h").write_text(HEADER)
    (tmp_path / "near.h").write_text("static inline int nr(void *p) { return p + 4 != 0; }\n")
    body = '#include "h.h"\nint g(void *p) { return hf(p) + hf(p) + hs(p) + before(p) + nr(p); }\n'
    proc = _run(tmp_path, body, "-Wall", "-Wpointer-arith", "-Wstrict-prototypes", opt)
    assert proc.returncode == 0, proc.stderr
    assert _diags(proc) == [("h.h:1", ARITH)], proc.stderr


def test_no_ignored_warning(tmp_path):
    (tmp_path / "h.h").write_text("#pragma GCC system_header\nint x;\n")
    proc = _run(tmp_path, '#include "h.h"\n', "-Wall")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_through_pragma_operator(tmp_path):
    (tmp_path / "h.h").write_text('_Pragma("GCC system_header")\nint q(void *p) { return p + 1 != 0; }\n')
    proc = _run(tmp_path, '#include "h.h"\n', "-Wpointer-arith")
    assert _diags(proc) == [], proc.stderr


def test_main_file_is_not_a_system_header(tmp_path):
    proc = _run(tmp_path, "#pragma GCC system_header\nvoid *f(void *p) { return p + 1; }\n", "-Wall", "-Wpointer-arith")
    assert _diags(proc) == [
        ("t.c:1", "#pragma GCC system_header ignored outside include file"),
        ("t.c:2", ARITH),
    ], proc.stderr


def test_ignored_attributes_too(tmp_path):
    (tmp_path / "h.h").write_text(
        "#pragma GCC system_header\nint lg(const char *f, ...) __attribute__((format(printf, 1, 2)));\n"
    )
    proc = _run(tmp_path, '#include "h.h"\n', "-Wignored-attributes")
    assert _diags(proc) == [], proc.stderr


def test_preprocess_writes_flag_3(tmp_path):
    """tcc -E: the line goes and the line markers say it, as gcc's
    (test_preprocess_line_markers.py)."""
    (tmp_path / "h.h").write_text("#pragma GCC system_header\nint x;\n")
    src = tmp_path / "t.c"
    src.write_text('#include "h.h"\n')
    proc = subprocess.run([str(TCC), "-ccompat", "-E", str(src)], capture_output=True, text=True)
    assert proc.returncode == 0 and "#pragma GCC system_header" not in proc.stdout, proc.stderr
    assert 'h.h" 3\nint x;' in proc.stdout, proc.stdout
