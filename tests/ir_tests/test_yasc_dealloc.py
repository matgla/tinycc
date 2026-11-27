"""yasc `owned(dealloc)`: ownership that names its deallocator
(docs/static_analysis_todo.md).

`FILE *?owned(fclose) fopen(...)`, or `malloc(fclose, 1)` on a
regular-C allocator, says what releases the pointer.  A plain `owned`
variable takes on the deallocator of what it holds; handed to a `drop`
parameter of another deallocator -- the parameter's own owned(...), else
the callee's name -- it is reported: "'f' is released by fclose, not free".
Plain `owned` says nothing and is never compared.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
typedef struct FILE FILE;
void free(drop void *?owned p);
int fclose(drop FILE *owned(fclose) f);
FILE *?owned(fclose) fopen(const char *p, const char *m);
void *?owned malloc(unsigned n);
void mem_free(void *p);
void *?owned mem_new(unsigned n) __attribute__((malloc, malloc(mem_free, 1)));
void release(drop void *?owned p);
"""
PRELUDE_LINES = PRELUDE.count("\n")


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
    "variable": ("void f(void)\n{\n  FILE *?owned f = fopen(\"a\", \"r\");\n  if (f)\n    free(f);\n}\n", 5,
                 "'f' is released by fclose, not free"),
    "attribute": ("void f(void)\n{\n  void *?owned p = mem_new(4);\n  free(p);\n}\n", 4,
                  "'p' is released by mem_free, not free"),
    "declared": ("void f(void)\n{\n  FILE *?owned(fclose) f = 0;\n  release(f);\n}\n", 4,
                 "'f' is released by fclose, not release"),
    "call_result": ("void f(void)\n{\n  free(fopen(\"x\", \"r\"));\n}\n", 3, "a pointer released by fclose is passed to free"),
    # move(p) is a copy of p: the deallocator goes with it
    "move": ("void f(void)\n{\n  FILE *?owned f = fopen(\"a\", \"r\");\n  if (f)\n    free(move(f));\n}\n", 5,
             "'f' is released by fclose, not free"),
    "move_attribute": ("void f(void)\n{\n  void *?owned p = mem_new(4);\n  free(move(p));\n}\n", 4,
                       "'p' is released by mem_free, not free"),
    "move_declared": ("void f(void)\n{\n  FILE *?owned(fclose) f = fopen(\"a\", \"r\");\n  release(move(f));\n}\n", 4,
                      "'f' is released by fclose, not release"),
    "move_to_attribute_dealloc": ("void f(void)\n{\n  FILE *?owned f = fopen(\"a\", \"r\");\n  mem_free(move(f));\n}\n", 4,
                                  "'f' is released by fclose, not mem_free"),
    "move_into_variable": ("void f(void)\n{\n  FILE *?owned f = fopen(\"a\", \"r\");\n"
                           "  void *?owned q = move(f);\n  free(q);\n}\n", 5, "'q' is released by fclose, not free"),
    "move_into_declared": ("void f(void)\n{\n  void *?owned p = mem_new(4);\n"
                           "  FILE *?owned(fclose) g = move(p);\n  fclose(g);\n}\n", 4,
                           "a pointer released by mem_free is stored in 'g', which is released by fclose"),
}


@pytest.mark.parametrize("name", sorted(BAD))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_wrong_deallocator(tmp_path, name, opt):
    body, line, msg = BAD[name]
    assert (line, msg) in _diags(tmp_path, body, opt)


CLEAN = {
    "fclose": "void f(void)\n{\n  FILE *?owned f = fopen(\"a\", \"r\");\n  if (f)\n    fclose(f);\n}\n",
    "auto": "void f(void)\n{\n  auto f = fopen(\"a\", \"r\");\n  if (f)\n    fclose(f);\n}\n",
    "plain_owned_anywhere": "void f(void)\n{\n  void *?owned p = malloc(4);\n  free(p);\n  release(malloc(2));\n}\n",
    "attribute_pair": "void f(void)\n{\n  void *?owned p = mem_new(4);\n  mem_free(p);\n}\n",
    "move_fclose": "void f(void)\n{\n  FILE *?owned f = fopen(\"a\", \"r\");\n  if (f)\n    fclose(move(f));\n}\n",
    "move_attribute_pair": "void f(void)\n{\n  void *?owned p = mem_new(4);\n  mem_free(move(p));\n}\n",
    "move_plain_owned": "void f(void)\n{\n  void *?owned p = malloc(4);\n  release(move(p));\n}\n",
    "move_through_variable": "void f(void)\n{\n  FILE *?owned f = fopen(\"a\", \"r\");\n"
                             "  FILE *?owned g = move(f);\n  if (g)\n    fclose(move(g));\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_right_deallocator(tmp_path, name, opt):
    assert [d for d in _diags(tmp_path, CLEAN[name], opt) if "released" in d[1]] == []


def test_declarators(tmp_path):
    body = "char *owned (*fp)(void);\nchar *?owned(free) g(void);\nvoid h(char *owned(x) *p);\n"
    assert _diags(tmp_path, body) == []
