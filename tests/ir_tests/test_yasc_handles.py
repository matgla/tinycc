"""yasc int handles: `int owned(close) maybe fd = open(...)`
(docs/static_analysis_todo.md).

Ownership for ints, only where written (a plain int stays plain): the int
type's ref is a handle node with the qualifiers.  `maybe` on a handle: it may
hold the invalid value (< 0); `fd < 0`, `fd == -1`, `fd >= 0`, `fd != -1`
refine it as a null test does a pointer (`if (fd)` does not: 0 is a valid
handle).  A maybe handle passed to an `int view fd` parameter warns; a view
does not escape; `int close(int drop owned(close) fd)` releases it; leaks,
double release, discarded results, move(fd) and -Wimplicit-move work as for
pointers.  A constant (-1) owns nothing.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
int owned(close) maybe open(const char *p, int flags);
int close(int drop owned(close) fd);
int read(int view fd, char * maybe buf, int n);
int keep_fd(int owned(close) fd);
int g_fd;
"""
PRELUDE_LINES = PRELUDE.count("\n")
OPEN = "void f(void)\n{\n  int owned(close) maybe fd = open(\"a\", 0);\n"
LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _diags(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-Wlifetime", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0, proc.stderr
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "note"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[1], parts[2]))
    return out


CLEAN = {
    "tested_lt": OPEN + "  if (fd < 0)\n    return;\n  read(fd, 0, 0);\n  close(fd);\n}\n",
    "tested_ge": OPEN + "  if (fd >= 0)\n  {\n    read(fd, 0, 0);\n    close(fd);\n  }\n}\n",
    "tested_eq_minus_one": OPEN + "  if (fd == -1)\n    return;\n  close(fd);\n}\n",
    "auto_and_move": "void f(void)\n{\n  auto fd = open(\"a\", 0);\n  if (fd != -1)\n    keep_fd(move(fd));\n}\n",
    "constants": "void f(void)\n{\n  int owned(close) maybe fd = -1;\n  read(0, 0, 0);\n  (void)fd;\n}\n",
    "plain_int_is_plain": "int f(int x)\n{\n  int y = x + 1;\n  return read(y, 0, 0);\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_clean(tmp_path, name, opt):
    assert _diags(tmp_path, CLEAN[name], opt) == []


BAD = {
    "untested_view_argument": (OPEN + "  read(fd, 0, 0);\n  if (fd >= 0)\n    close(fd);\n}\n", 4,
                               "a handle that may be invalid is passed as argument 1 of 'read', which is not maybe"),
    "leak": (OPEN + "  if (fd < 0)\n    return;\n  read(fd, 0, 0);\n}\n", 7,
             "'fd' leaks: it still owns a handle at the end of its scope"),
    "released_twice": (OPEN + "  if (fd == -1)\n    return;\n  close(fd);\n  close(fd);\n}\n", 7, "'fd' is released twice"),
    "zero_test_is_not_validity": (OPEN + "  if (fd)\n    read(fd, 0, 0);\n  if (fd >= 0)\n    close(fd);\n}\n", 5,
                                  "a handle that may be invalid is passed as argument 1 of 'read', which is not maybe"),
    "view_escapes": ("void f(int view fd)\n{\n  g_fd = fd;\n}\n", 3,
                     "'fd' is a view: it cannot leave the function (it is stored in global 'g_fd')"),
    "implicit_move": (OPEN + "  if (fd >= 0)\n    keep_fd(fd);\n}\n", 5, "ownership of 'fd' moves here: write move(fd)"),
    "stored_in_plain_int": ("void f(void)\n{\n  int x = open(\"a\", 0);\n  (void)x;\n}\n", 3,
                            "an owned result is stored in an int that owns nothing: it is never released"),
    "discarded": ("void f(void)\n{\n  open(\"a\", 0);\n}\n", 3, "an owned result is discarded: it is never released"),
}


@pytest.mark.parametrize("name", sorted(BAD))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_reported(tmp_path, name, opt):
    body, line, msg = BAD[name]
    assert (line, "warning", msg) in _diags(tmp_path, body, opt)


def test_wrong_deallocator(tmp_path):
    body = "void free(drop void * owned maybe p);\nvoid f(void)\n{\n  int owned(close) maybe fd = open(\"a\", 0);\n" \
           "  if (fd >= 0)\n    close(fd);\n}\n"
    assert [d for d in _diags(tmp_path, body) if "released by" in d[2]] == []


def test_handle_declarations(tmp_path):
    body = "int f(int owned);\ntypedef int owned(close) fd_t;\nfd_t g(void);\nint h(int view, int owned(close) maybe);\n"
    assert _diags(tmp_path, body) == []


def test_takes_a_handle(tmp_path):
    """fdopen-like: the stream takes the descriptor over; null: still the caller's."""
    body = (
        "typedef struct F F;\nint fcl(drop F * owned(fcl) f);\n"
        "F * owned(fcl) maybe takes(fd) wrap(int owned(close) fd);\n"
        "void f(void)\n{\n  auto fd = open(\"a\", 0);\n  if (fd < 0)\n    return;\n"
        "  F * owned maybe s = wrap(move(fd));\n  if (!s)\n  {\n    close(fd);\n    return;\n  }\n  fcl(s);\n}\n"
    )
    assert _diags(tmp_path, body) == []
