"""-iyasc DIR: a yasc include path.  Searched in its place among the -I
directories; a header found there is a yasc file, as if it began with
#pragma yasc (so is a "..." header next to it), whoever includes it -- a
regular-C main file too.  YASC_INCLUDE_PATH (':'-separated, as CPATH) adds
them from the environment, after the command line's.  tcc -E writes the
#pragma yasc such a header stands for, so its output compiles the same.
"""

import os
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

YASC_H = """\
char * owned yalloc(void);
void yfree(drop char * owned p);
#include "ynext.h"
static inline int ylen(char * view s) { return s[0]; }
"""
YNEXT_H = "char * borrows(static) ynext(void);\n"
C_H = "int view(int owned);\n"

MAIN = """\
#include <c.h>
#include <ya.h>
int f(void)
{
  char * owned p = yalloc();
  int r = ylen(p) + view(1) + ylen(ynext()) + ylen(ynext());
  yfree(move(p));
  return r;
}
"""


def _setup(tmp_path):
    (tmp_path / "yinc").mkdir()
    (tmp_path / "cinc").mkdir()
    (tmp_path / "yinc" / "ya.h").write_text(YASC_H)
    (tmp_path / "yinc" / "ynext.h").write_text(YNEXT_H)
    (tmp_path / "cinc" / "c.h").write_text(C_H)
    (tmp_path / "m.c").write_text(MAIN)


def _tcc(tmp_path, *args, env=None):
    return subprocess.run([str(TCC), "-Wno-missing-prototypes", *args], capture_output=True, text=True, cwd=tmp_path, env=env)


@pytest.mark.parametrize("opt", LEVELS)
def test_headers_are_yasc(tmp_path, opt):
    _setup(tmp_path)
    proc = _tcc(tmp_path, "-iyasc", "yinc", "-Icinc", opt, "-c", "m.c", "-o", "m.o")
    assert proc.returncode == 0, proc.stderr
    assert not proc.stderr, proc.stderr


def test_same_headers_through_I_are_regular_c(tmp_path):
    _setup(tmp_path)
    proc = _tcc(tmp_path, "-Iyinc", "-Icinc", "-c", "m.c", "-o", "m.o")
    assert proc.returncode != 0
    assert "ya.h:1: error" in proc.stderr, proc.stderr


def test_checked_as_yasc(tmp_path):
    """A plain pointer in an -iyasc header is the yasc error."""
    _setup(tmp_path)
    (tmp_path / "yinc" / "ynext.h").write_text("char *ynext(void);\n")
    proc = _tcc(tmp_path, "-iyasc", "yinc", "-Icinc", "-c", "m.c", "-o", "m.o")
    assert proc.returncode != 0
    assert "ynext.h:1: error: plain pointer in yasc: say owned or view" in proc.stderr, proc.stderr


def test_regular_c_main_file(tmp_path):
    """Under -ccompat the main file is regular C, the -iyasc header is yasc."""
    _setup(tmp_path)
    (tmp_path / "m.c").write_text('#include <ya.h>\nint owned(char *view) { return ylen(view); }\n')
    proc = _tcc(tmp_path, "-ccompat", "-Wno-lifetime", "-iyasc", "yinc", "-c", "m.c", "-o", "m.o")
    assert proc.returncode == 0, proc.stderr


def test_search_order_follows_the_command_line(tmp_path):
    """-I before -iyasc: the -I copy of a header wins and is regular C."""
    _setup(tmp_path)
    (tmp_path / "cinc" / "ya.h").write_text("int owned;\n")
    (tmp_path / "m.c").write_text("#include <ya.h>\nint g(void) { return 0; }\n")
    proc = _tcc(tmp_path, "-Icinc", "-iyasc", "yinc", "-c", "m.c", "-o", "m.o")
    assert proc.returncode == 0, proc.stderr
    proc = _tcc(tmp_path, "-iyasc", "yinc", "-Icinc", "-c", "m.c", "-o", "m.o")
    assert proc.returncode == 0, proc.stderr
    # and the yasc copy is the one read: declares yalloc
    (tmp_path / "m.c").write_text("#include <ya.h>\nint g(void) { return owned; }\n")
    proc = _tcc(tmp_path, "-iyasc", "yinc", "-Icinc", "-c", "m.c", "-o", "m.o")
    assert proc.returncode != 0


def test_includer_dialect_unchanged(tmp_path):
    """The file including an -iyasc header stays what it was after it."""
    _setup(tmp_path)
    (tmp_path / "cinc" / "c2.h").write_text("#include <ya.h>\nint out;\n")
    (tmp_path / "m.c").write_text("#include <c2.h>\nint g(void) { return out; }\n")
    proc = _tcc(tmp_path, "-ccompat", "-iyasc", "yinc", "-Icinc", "-c", "m.c", "-o", "m.o")
    assert proc.returncode == 0, proc.stderr


def test_environment(tmp_path):
    _setup(tmp_path)
    env = dict(os.environ, YASC_INCLUDE_PATH=f"{tmp_path / 'nothing'}:{tmp_path / 'yinc'}")
    proc = _tcc(tmp_path, "-Icinc", "-c", "m.c", "-o", "m.o", env=env)
    assert proc.returncode == 0, proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_preprocessed_output(tmp_path, opt):
    """tcc -E writes #pragma yasc at the start of each such header: the output
    compiles to the same object."""
    _setup(tmp_path)
    pre = _tcc(tmp_path, "-iyasc", "yinc", "-Icinc", "-E", "m.c", "-o", "m.i")
    assert pre.returncode == 0, pre.stderr
    text = (tmp_path / "m.i").read_text()
    assert text.count("#pragma yasc") == 2, text
    a = _tcc(tmp_path, "-iyasc", "yinc", "-Icinc", opt, "-c", "m.c", "-o", "a.o")
    b = _tcc(tmp_path, opt, "-c", "m.i", "-o", "b.o")
    assert a.returncode == 0, a.stderr
    assert b.returncode == 0, b.stderr
    # the line numbers survive the extra line
    (tmp_path / "yinc" / "ynext.h").write_text("\n\nchar *ynext(void);\n")
    assert _tcc(tmp_path, "-iyasc", "yinc", "-Icinc", "-E", "m.c", "-o", "m.i").returncode == 0
    b = _tcc(tmp_path, opt, "-c", "m.i", "-o", "b.o")
    assert "ynext.h:3: error: plain pointer in yasc" in b.stderr, b.stderr


def test_has_include_does_not_mark(tmp_path):
    _setup(tmp_path)
    (tmp_path / "m.c").write_text("#if __has_include(<ya.h>)\nint owned = 1;\n#endif\n")
    proc = _tcc(tmp_path, "-ccompat", "-iyasc", "yinc", "-c", "m.c", "-o", "m.o")
    assert proc.returncode == 0, proc.stderr
