"""yasc: owned items of local structs and arrays (`b.buf`, `arr[0]`,
`fds[1]`), followed by frame offset (lifetime.c, tcc_ir_slot_check).

They have no vreg: the frontend records each owned pointer or int handle
inside a local struct or array (arrays of at most 16 elements, structs
nested a few levels).  Stores, reads (folded loads too), arguments, moves
into a variable, the end of the local's scope and the local's address
passed to an owned out-parameter (`pipe(p)`, a status result's test
deciding) are followed; a local whose address goes anywhere else, or that
is indexed with a value that is neither a constant nor a variable (plus a
constant: `a[i + 1]`), such as `a[i * 2]`, is not.  Elements at a variable
index: test_lifetime_index.py.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
void *?owned malloc(unsigned n);
void free(drop void *?owned p);
int close(int drop ?owned(close) fd);
int ?owned(close) open(const char *p, int f);
int pipe(out int ?owned(close) *fds);
int use(char *p);
struct box { char *?owned buf; int n; };
void touch(inout struct box *b);
"""
PRELUDE_LINES = PRELUDE.count("\n")
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
        if len(parts) == 3 and parts[1] == "warning":
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[2]))
    return out


BAD = {
    # not followed, and said so
    "computed_index": ("void f(int i)\n{\n  char *?owned a[4];\n  a[i * 2] = malloc(4);\n}\n", 4,
                       "'a' is reached in a way the lifetime check does not follow: what it owns is not checked"),
    "struct_member_leaks": ("void f(void)\n{\n  struct box b = undefined;\n  b.buf = malloc(4);\n}\n", 5,
                            "'b.buf' leaks: it still owns memory at the end of its scope"),
    # an index variable plus a constant: its own element (test_lifetime_index.py)
    "offset_index_leaks": ("void f(int i)\n{\n  char *?owned a[4];\n  a[i + 1] = malloc(4);\n}\n", 5,
                           "'a[i + 1]' leaks: it may still own memory at the end of its scope"),
    "array_element_leaks": ("void f(void)\n{\n  char *?owned a[4];\n  a[0] = malloc(4);\n}\n", 5,
                            "'a[0]' leaks: it still owns memory at the end of its scope"),
    "handle_array_leaks": ("void f(void)\n{\n  int ?owned(close) fds[2];\n  fds[0] = open(\"a\", 0);\n}\n", 5,
                           "'fds[0]' leaks: it still owns a handle at the end of its scope"),
    "freed_twice": ("void f(void)\n{\n  struct box b = undefined;\n  b.buf = malloc(4);\n  free(b.buf);\n  free(b.buf);\n}\n", 6,
                    "'b.buf' is freed twice"),
    "used_after_free": ("void f(void)\n{\n  struct box b = undefined;\n  b.buf = malloc(4);\n  free(b.buf);\n  use(b.buf);\n}\n", 6,
                        "'b.buf' is used after it was freed"),
    "maybe_untested": ("void f(void)\n{\n  struct box b = undefined;\n  b.buf = malloc(4);\n  use(b.buf);\n  free(b.buf);\n}\n", 5,
                       "'b.buf' may be null, and is passed as argument 1 of 'use', which is not '?'"),
    "pipe_half_closed": ("void f(void)\n{\n  int ?owned(close) p[2] = undefined;\n  if (pipe(p) < 0)\n    return;\n"
                         "  close(p[0]);\n}\n", 7, "'p[1]' leaks: it still owns a handle at the end of its scope"),
    "assigned_again": ("void f(void)\n{\n  struct box b = undefined;\n  b.buf = malloc(4);\n  b.buf = malloc(8);\n  free(b.buf);\n}\n",
                       5, "'b.buf' leaks: it still owns memory when it is assigned again"),
    "variable_index": ("void f(int i)\n{\n  char *?owned a[4];\n  a[i] = malloc(4);\n}\n", 5,
                       "'a[i]' leaks: it may still own memory at the end of its scope"),
    "handed_to_view_leaks": ("void f(void)\n{\n  struct box b = undefined;\n  b.buf = malloc(4);\n  touch(&b);\n}\n", 6,
                             "'b.buf' leaks: it may still own memory at the end of its scope"),
    "inner_scope": ("void f(int x)\n{\n  if (x)\n  {\n    struct box b = undefined;\n    b.buf = malloc(4);\n  }\n  x = 1;\n}\n",
                    8, "'b.buf' leaks: it still owns memory at the end of its scope"),
}


@pytest.mark.parametrize("name", sorted(BAD))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_reported(tmp_path, name, opt):
    body, line, msg = BAD[name]
    assert (line, msg) in _diags(tmp_path, body, opt)


CLEAN = {
    "freed": "void f(void)\n{\n  struct box b = undefined;\n  b.buf = malloc(4);\n  free(b.buf);\n}\n",
    "tested": "void f(void)\n{\n  struct box b = undefined;\n  b.buf = malloc(4);\n  if (b.buf)\n    use(b.buf);\n  free(b.buf);\n}\n",
    "pipe_both_closed": "void f(void)\n{\n  int ?owned(close) p[2] = undefined;\n  if (pipe(p) < 0)\n    return;\n"
    "  close(p[0]);\n  close(p[1]);\n}\n",
    "moved_out": "void f(void)\n{\n  struct box b = undefined;\n  b.buf = malloc(4);\n  char *?owned q = move(b.buf);\n"
    "  free(q);\n}\n",
    # handed to a method's `struct T *view self`: still followed, the callee
    # keeps the member valid (test_yasc_self.py)
    "handed_to_view_then_freed": "void f(void)\n{\n  struct box b = undefined;\n  b.buf = malloc(4);\n  touch(&b);\n"
    "  free(b.buf);\n}\n",
    "null_store": "void f(void)\n{\n  struct box b = undefined;\n  b.buf = 0;\n  (void)b;\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", LEVELS)
def test_clean(tmp_path, name, opt):
    assert _diags(tmp_path, CLEAN[name], opt) == []
