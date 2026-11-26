"""yasc `owned(dealloc)`: ownership that names its deallocator
(docs/static_analysis_todo.md).

`FILE * owned(fclose) maybe fopen(...)`, or `malloc(fclose, 1)` on a
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
void free(drop void * owned maybe p);
int fclose(drop FILE * owned(fclose) f);
FILE * owned(fclose) maybe fopen(const char *p, const char *m);
void * owned maybe malloc(unsigned n);
void mem_free(void *p);
void * owned maybe mem_new(unsigned n) __attribute__((malloc, malloc(mem_free, 1)));
void release(drop void * owned maybe p);
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
    "variable": ("void f(void)\n{\n  FILE * owned maybe f = fopen(\"a\", \"r\");\n  if (f)\n    free(f);\n}\n", 5,
                 "'f' is released by fclose, not free"),
    "attribute": ("void f(void)\n{\n  void * owned maybe p = mem_new(4);\n  free(p);\n}\n", 4,
                  "'p' is released by mem_free, not free"),
    "declared": ("void f(void)\n{\n  FILE * owned(fclose) maybe f = 0;\n  release(f);\n}\n", 4,
                 "'f' is released by fclose, not release"),
    "call_result": ("void f(void)\n{\n  free(fopen(\"x\", \"r\"));\n}\n", 3, "a pointer released by fclose is passed to free"),
}


@pytest.mark.parametrize("name", sorted(BAD))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_wrong_deallocator(tmp_path, name, opt):
    body, line, msg = BAD[name]
    assert (line, msg) in _diags(tmp_path, body, opt)


CLEAN = {
    "fclose": "void f(void)\n{\n  FILE * owned maybe f = fopen(\"a\", \"r\");\n  if (f)\n    fclose(f);\n}\n",
    "auto": "void f(void)\n{\n  auto f = fopen(\"a\", \"r\");\n  if (f)\n    fclose(f);\n}\n",
    "plain_owned_anywhere": "void f(void)\n{\n  void * owned maybe p = malloc(4);\n  free(p);\n  release(malloc(2));\n}\n",
    "attribute_pair": "void f(void)\n{\n  void * owned maybe p = mem_new(4);\n  mem_free(p);\n}\n",
}


@pytest.mark.parametrize("name", sorted(CLEAN))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_right_deallocator(tmp_path, name, opt):
    assert [d for d in _diags(tmp_path, CLEAN[name], opt) if "released" in d[1]] == []


def test_declarators(tmp_path):
    body = "char * owned (*fp)(void);\nchar * owned(free) maybe g(void);\nvoid h(char * owned(x) *p);\n"
    assert _diags(tmp_path, body) == []
