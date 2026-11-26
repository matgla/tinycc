"""yasc int handles as struct members: `int owned(close) maybe fd;`.

They parse in a struct body (owned(...) is a qualifier there, never a
method's name), a union refuses them as it refuses owned pointers, and the
member paths of -Wlifetime follow them as they follow `* owned maybe`
members: a local struct's handle leaks, a member closed twice or used after
it was closed is said, an untested one passed to a non-maybe parameter is
said, and `c->fd >= 0` / `< 0` / `== -1` tests refine it.  The same at every
-O level.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

PRELUDE = """\
int close(drop int owned maybe fd);
int owned(close) maybe open(const char *p, int fl);
long write(int view fd, const void *b, unsigned n);
struct conn { int owned(close) maybe fd; int n; };
struct m { int owned(close) maybe fd; int get(struct m *self); };
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _compile(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-c", "-fmax-errors=0", "-Wno-unused-but-set-variable", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            where = parts[0].rsplit(":", 1)
            n = int(where[1]) - PRELUDE_LINES if len(where) == 2 and where[1].isdigit() else 0
            out.append((n, parts[1], parts[2]))
    return out


MAYBE_ARG = "a handle that may be invalid is passed as argument 1 of 'write', which is not maybe"

BAD = {
    "leak": ("void f(void) { struct conn c = undefined; c.fd = open(\"x\", 0); }", "'c.fd' leaks: it still owns a handle at the end of its scope"),
    "twice": ("void f(inout struct conn *c) { close(c->fd); close(c->fd); }", "'c->fd' is released twice"),
    "after": ("void f(inout struct conn *c) { if (c->fd < 0) return; close(c->fd); write(c->fd, \"x\", 1); }",
              "'c->fd' is used after it was released"),
    "untested": ("void f(struct conn *c) { write(c->fd, \"x\", 1); }", MAYBE_ARG),
    "plain_int": ("void f(inout struct conn *c, int fd) { c->fd = fd; }", "an int that owns nothing is stored in an owned handle"),
    "local_twice": ("void f(void) { struct conn c = undefined; c.fd = open(\"x\", 0); close(c.fd); close(c.fd); }", "'c.fd' is released twice"),
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(BAD))
def test_handle_member_errors(tmp_path, name, opt):
    body, msg = BAD[name]
    proc = _compile(tmp_path, body + "\n", opt)
    assert proc.returncode != 0
    errors = [d for d in _diags(proc) if d[1] == "error"]
    assert (1, "error", msg) in errors, proc.stderr
    assert len([d for d in errors if d[2] == msg]) == 1, proc.stderr


GOOD = {
    "tested": "void f(struct conn *c) { if (c->fd >= 0) write(c->fd, \"x\", 1); }",
    "early_return": "void f(struct conn *c) { if (c->fd < 0) return; write(c->fd, \"x\", 1); }",
    "minus_one": "void f(struct conn *c) { if (c->fd == -1) return; write(c->fd, \"x\", 1); }",
    "reopen": "void f(inout struct conn *c) { close(c->fd); c->fd = open(\"y\", 0); close(c->fd); c->fd = -1; }",
    "closed_and_cleared": "void f(inout struct conn *c) { close(c->fd); c->fd = -1; }",
    "local_closed": "void f(void) { struct conn c = undefined; c.fd = open(\"x\", 0); close(c.fd); }",
    "method": "int m__get(struct m *self) { return cast<as(int)>(self->fd); }",  # its number
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(GOOD))
def test_handle_member_fine(tmp_path, name, opt):
    proc = _compile(tmp_path, GOOD[name] + "\n", opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_handle_member_in_union(tmp_path):
    proc = _compile(tmp_path, "union bad { int owned(close) fd; int x; };\n")
    msg = "a union holds an owned member 'fd': which member is live is not known, so it is never freed"
    assert (1, "error", msg) in _diags(proc), proc.stderr
