"""-Wlifetime: which calls forget what was known about globals.

Every call used to forget the member paths of global objects, so
`free(g.p); free(g.p);` was not said: free is a call.  Now:
- a direct call to a prototype a system header gives (libc, the trusted
  layer), with no function pointer parameter, writes only the globals
  system headers declare (errno, stdout) -- qsort / atexit run user code and
  still forget everything;
- a callee with __attribute__((leaf)) cannot reach this unit's static
  globals;
- any other call, and any call through a pointer, forgets every global.
A libc or leaf callee handed a pointer it may write through (not a local's
address, not an owned argument) may write a global through it: that call
forgets every global too.  The tagged-union check follows the same rule.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O2"]

SYS_HEADER = """\
#pragma yasc
void free(drop void * owned maybe p);
void qsort(inout void * maybe base, unsigned n, unsigned sz, int (*cmp)(const void *, const void *));
void lib_note(const char *s);
void * view memset(inout void *d, int c, unsigned n);
struct libstate { char * owned maybe buf; };
extern struct libstate lib_g;
"""

PRELUDE = """\
#include <mylibc.h>
struct s { char * owned maybe p; };
struct s g;
static struct s sg;
void user_fn(void);
__attribute__((leaf)) void leaf_fn(void);
void (*hook)(void);
static int cmp(const void *a, const void *b) { free(g.p); return 0; }
"""
PRELUDE_LINES = PRELUDE.count("\n")


def _compile(tmp_path, body, *flags):
    inc = tmp_path / "sys"
    inc.mkdir(exist_ok=True)
    (inc / "mylibc.h").write_text(SYS_HEADER)
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    return subprocess.run(
        [str(TCC), "-isystem", str(inc), "-c", "-fmax-errors=0", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _errors(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] == "error":
            where = parts[0].rsplit(":", 1)
            n = int(where[1]) - PRELUDE_LINES if len(where) == 2 and where[1].isdigit() else 0
            out.append((n, parts[2]))
    return out


SAID = {
    "libc_call": ("void f(void)\n{\n  free(g.p);\n  free(g.p);\n}\n", 4, "'g.p' is freed twice"),
    "libc_call_between": ("void f(void)\n{\n  free(g.p);\n  lib_note(\"x\");\n  free(g.p);\n}\n", 5, "'g.p' is freed twice"),
    "leaf_keeps_statics": ("void f(void)\n{\n  free(sg.p);\n  leaf_fn();\n  free(sg.p);\n}\n", 5, "'sg.p' is freed twice"),
    "libc_writes_a_local_array": (
        "void f(void)\n{\n  char buf[8];\n  free(g.p);\n  memset(buf, 0, sizeof buf);\n  free(g.p);\n}\n",
        6,
        "'g.p' is freed twice",
    ),
    "libc_writes_a_local_struct": (
        "void f(void)\n{\n  struct s l;\n  free(g.p);\n  memset(&l, 0, sizeof l);\n  free(g.p);\n}\n",
        6,
        "'g.p' is freed twice",
    ),
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(SAID))
def test_global_kept_across_the_call(tmp_path, name, opt):
    body, line, msg = SAID[name]
    proc = _compile(tmp_path, body, opt)
    assert (line, msg) in _errors(proc), proc.stderr


FORGOTTEN = {
    "callback": "void f(void)\n{\n  free(g.p);\n  qsort(0, 0, 0, cmp);\n  free(g.p);\n}\n",
    "user_function": "void f(void)\n{\n  free(g.p);\n  user_fn();\n  free(g.p);\n}\n",
    "through_a_pointer": "void f(void)\n{\n  free(g.p);\n  hook();\n  free(g.p);\n}\n",
    "leaf_and_an_extern_global": "void f(void)\n{\n  free(g.p);\n  leaf_fn();\n  free(g.p);\n}\n",
    "libc_global": "void f(void)\n{\n  free(lib_g.buf);\n  lib_note(\"x\");\n  free(lib_g.buf);\n}\n",
    "libc_through_an_alias": (
        "void f(void)\n{\n  struct s * view gp = &g;\n  free(g.p);\n  memset(gp, 0, sizeof *gp);\n  free(g.p);\n}\n"
    ),
    "libc_through_a_parameter": (
        "void f(inout struct s *sp)\n{\n  free(g.p);\n  memset(sp, 0, sizeof *sp);\n  free(g.p);\n}\n"
    ),
    "leaf_through_an_alias": (
        "void leaf_w(inout struct s *d) __attribute__((leaf));\n"
        "void f(void)\n{\n  struct s * view sp = &sg;\n  free(sg.p);\n  leaf_w(sp);\n  free(sg.p);\n}\n"
    ),
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(FORGOTTEN))
def test_global_forgotten_by_the_call(tmp_path, name, opt):
    proc = _compile(tmp_path, FORGOTTEN[name], opt)
    assert not [e for e in _errors(proc) if "freed twice" in e[1]], proc.stderr


def test_tagged_union_global_across_a_libc_call(tmp_path):
    body = (
        "enum vk { V_INT, V_STR };\n"
        "struct value { enum vk k; union(k) { long i when(V_INT); char * view s when(V_STR); } u; };\n"
        "struct value gv;\n"
        "long f(void)\n{\n  if (gv.k != V_INT)\n    return 0;\n  lib_note(\"x\");\n  return gv.u.i;\n}\n"
        "long h(void)\n{\n  if (gv.k != V_INT)\n    return 0;\n  user_fn();\n  return gv.u.i;\n}\n"
    )
    proc = _compile(tmp_path, body)
    errs = _errors(proc)
    assert (10, "'u.i' is used where 'k' may not be V_INT: test it first") not in errs, proc.stderr
    assert (16, "'u.i' is used where 'k' may not be V_INT: test it first") in errs, proc.stderr


def test_system_globals_of_an_earlier_unit_are_not_kept(tmp_path):
    """TCCState.sys_globals is by token number, and the numbers start again
    with each unit: the first unit's `extern int s, p, g;` from a system
    header is not the second unit's own `g`."""
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "mylibc.h").write_text(SYS_HEADER)
    (inc / "first.h").write_text("extern int s, p, g;\n")
    (tmp_path / "u1.c").write_text("#include <first.h>\nint u1(void) { return g; }\n")
    (tmp_path / "u2.c").write_text(
        "struct s { char * owned maybe p; } g;\n#include <mylibc.h>\n"
        "void f(void)\n{\n  free(g.p);\n  lib_note(\"x\");\n  free(g.p);\n}\n"
    )
    proc = subprocess.run(
        [str(TCC), "-isystem", str(inc), "-nostdlib", "-r", str(tmp_path / "u1.c"), str(tmp_path / "u2.c"),
         "-o", str(tmp_path / "both.o")],
        capture_output=True,
        text=True,
    )
    assert "u2.c:7: error: 'g.p' is freed twice" in proc.stderr, proc.stderr


def test_tagged_union_global_through_an_alias(tmp_path):
    body = (
        "enum vk { V_INT, V_STR };\n"
        "struct value { enum vk k; union(k) { long i when(V_INT); char * view s when(V_STR); } u; };\n"
        "struct value gv;\n"
        "long f(inout struct value *vp)\n{\n  if (gv.k != V_INT)\n    return 0;\n  memset(vp, 1, sizeof *vp);\n"
        "  return gv.u.i;\n}\n"
    )
    proc = _compile(tmp_path, body)
    assert (9, "'u.i' is used where 'k' may not be V_INT: test it first") in _errors(proc), proc.stderr
