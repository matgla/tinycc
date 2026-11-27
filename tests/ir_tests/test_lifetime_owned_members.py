"""-Wlifetime: an owned member that is never null (`T *owned buf`) is a
member path followed for ownership only (LT_MEMBER_NOTMAYBE): used or freed
after it was freed is reported, through a call's result it is said that the
check does not follow it -- and nothing about null, since it cannot be."""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

PRELUDE = """\
void *?owned malloc(unsigned n);
void free(drop void *?owned p);
int use(char *p);
struct box { char *owned buf; int n; };
struct box *borrows(static) holder(void);
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _diags(tmp_path, body, opt):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-c", "-fmax-errors=0", opt, str(src), "-o", str(tmp_path / "t.o")],
                          capture_output=True, text=True)
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[2]))
    return out


BAD = {
    "used_after_free": ("int f(inout struct box *b)\n{\n  free(b->buf);\n  return use(b->buf);\n}\n",
                        (4, "'b->buf' is used after it was freed")),
    "freed_twice": ("void f(inout struct box *b)\n{\n  free(b->buf);\n  free(b->buf);\n}\n",
                    (4, "'b->buf' is freed twice")),
    "through_call_result": ("void f(void)\n{\n  free(holder()->buf);\n}\n",
                            (3, "'buf' of a call's result is owned and the lifetime check does not follow it: "
                                "store the result in a variable")),
}

CLEAN = {
    "read": "int f(struct box *b)\n{\n  return use(b->buf) + b->buf[0];\n}\n",
    "replaced": "void f(inout struct box *b)\n{\n  char *?owned n = malloc(4);\n  if (!n)\n    return;\n"
    "  free(b->buf);\n  b->buf = move(n);\n  use(b->buf);\n}\n",
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(BAD))
def test_reported(tmp_path, name, opt):
    body, want = BAD[name]
    assert want in _diags(tmp_path, body, opt)


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(CLEAN))
def test_clean(tmp_path, name, opt):
    assert _diags(tmp_path, CLEAN[name], opt) == []
