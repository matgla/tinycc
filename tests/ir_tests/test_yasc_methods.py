"""yasc methods (docs/yasc_methods.md): what the compiler rejects, what the
ownership checks see through a method call, and the C header of a module that
exports a struct with methods.

The run-time behaviour is in yasc_methods.c and yasc_methods_mod*.c
(test_qemu.py, every -O level).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = "#pragma yasc\n"

# An allocator that says what it does, in a regular C header, as
# examples/yasc/mem.h does.
MEM_H = """
void mem_free(void *p);
void *mem_new(unsigned long n) __attribute__((malloc, malloc(mem_free, 1)));
"""


def _compile(tmp_path, body, *flags, name="t.c"):
    src = tmp_path / name
    src.write_text(body)
    return subprocess.run(
        [str(TCC), "-c", *flags, str(src), "-o", str(tmp_path / (src.stem + ".o"))],
        capture_output=True,
        text=True,
        cwd=tmp_path,
    )


def _yasc(tmp_path, body, *flags):
    return _compile(tmp_path, PRELUDE + body, "-ccompat", *flags)


def _error(proc):
    assert proc.returncode != 0, "expected a compile error"
    lines = [line.split("error: ", 1)[1] for line in proc.stderr.splitlines() if "error: " in line]
    assert lines, proc.stderr
    return lines[0]


def _warnings(proc):
    assert proc.returncode == 0, proc.stderr
    return [line.split("warning: ", 1)[1] for line in proc.stderr.splitlines() if "warning: " in line]


VEC = """
struct vec
{
  int x, y;
  int len2(const struct vec *self) { return self->x * self->x + self->y * self->y; }
  struct vec make(int x, int y) { return (struct vec){x, y}; }
};
"""


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_methods_compile_clean(tmp_path, opt):
    proc = _yasc(tmp_path, VEC + "int f(void) { struct vec v = vec.make(1, 2); return v.len2(); }\n", opt)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_method_symbols(tmp_path):
    proc = _yasc(tmp_path, VEC)
    assert proc.returncode == 0, proc.stderr
    nm = subprocess.run(["arm-none-eabi-nm", str(tmp_path / "t.o")], capture_output=True, text=True)
    if nm.returncode != 0:
        pytest.skip("needs arm-none-eabi-nm")
    names = {line.split()[-1] for line in nm.stdout.splitlines()}
    assert {"vec__len2", "vec__make"} <= names, nm.stdout


def test_no_self_needs_the_tag(tmp_path):
    proc = _yasc(tmp_path, VEC + "int f(void) { struct vec v = {1, 2}; v = v.make(3, 4); return v.x; }\n")
    assert _error(proc) == "method 'make' of struct vec has no self parameter: call it as vec.make(...)"


def test_unknown_method(tmp_path):
    proc = _yasc(tmp_path, VEC + "int f(void) { return vec.nope(); }\n")
    assert _error(proc) == "struct vec has no method 'nope'"


def test_unknown_member_is_still_a_field_error(tmp_path):
    proc = _yasc(tmp_path, VEC + "int f(void) { struct vec v = {1, 2}; return v.z; }\n")
    assert _error(proc) == "field not found: z"


def test_method_not_called(tmp_path):
    proc = _yasc(tmp_path, VEC + "int f(void) { struct vec v = {1, 2}; int (*p)(const struct vec *view) = v.len2; return 0; }\n")
    assert _error(proc) == "method 'len2' of struct vec is not called: write vec.len2 for the function itself"


def test_field_and_method_share_a_name(tmp_path):
    proc = _yasc(tmp_path, "struct v { int x; int x(const struct v *s) { return 0; } };\n")
    assert _error(proc) == "struct v: 'x' is both a field and a method"


def test_method_declared_twice(tmp_path):
    proc = _yasc(tmp_path, "struct v { int x; int m(int a) { return a; } int m(int b) { return b; } };\n")
    assert _error(proc) == "struct v: method 'm' declared twice"


def test_unnamed_struct(tmp_path):
    proc = _yasc(tmp_path, "typedef struct { int x; int m(int a) { return a; } } T;\n")
    assert _error(proc) == "method 'm' in an unnamed struct: give the struct a tag"


def test_struct_in_a_function(tmp_path):
    proc = _yasc(tmp_path, "int f(void) { struct v { int x; int m(int a) { return a; } }; return 0; }\n")
    assert _error(proc) == "method 'm': a struct with methods must be defined at file scope"


def test_struct_in_a_struct(tmp_path):
    proc = _yasc(tmp_path, "struct o { struct v { int x; int m(int a) { return a; } } in; };\n")
    assert _error(proc) == "method 'm': a struct with methods cannot be defined inside another struct"


def test_regular_c_has_no_methods(tmp_path):
    proc = _compile(tmp_path, "struct v { int x; int m(int a) { return a; } };\n", "-ccompat")
    assert _error(proc) == "invalid type for 'm'"


def test_fields_are_unchanged(tmp_path):
    """Function pointers, bit-fields, arrays, nested structs and _Static_assert
    are fields as before."""
    proc = _yasc(
        tmp_path,
        "int g(int a) { return a; }\n"
        "struct v { int x; int (*cb)(int); int arr[2]; unsigned b : 3; struct { int in; } s;"
        " _Static_assert(1, \"ok\"); int m(const struct v *self) { return self->cb(self->x); } };\n"
        "int f(void) { struct v a = { 1, g }; return a.cb(2) + a.m() + (int)sizeof a; }\n",
    )
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_typedef_name_starts_a_call(tmp_path):
    proc = _yasc(tmp_path, "typedef struct v { int x; void reset(void) { } } v;\nvoid f(void) { v.reset(); v a = {1}; (void)a; }\n")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_variable_shadows_the_tag(tmp_path):
    """`vec.x` with a variable named vec in scope is that variable's field."""
    proc = _yasc(tmp_path, VEC + "int f(void) { struct vec vec = {2, 3}; return vec.x + vec.len2(); }\n")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


# Ownership checks see a method call as the call it is: the object is the
# first argument, so `drop ... owned self` consumes the pointer.
BUF = """#include "mem.h"
struct buf
{
  char *owned data;
  int len;

  struct buf *?owned create(int n)
  {
    struct buf *?owned b = mem_new(sizeof *b);
    if (!b)
      return b;
    char *?owned d = mem_new(n);
    if (!d)
    {
      mem_free(b);
      return 0;
    }
    b->data = move(d);
    b->len = n;
    return b;
  }
  int size(const struct buf *self) { return self->len; }
  void destroy(drop struct buf *owned self)
  {
    mem_free(self->data);
    mem_free(self);
  }
};
"""

OPEN = "  struct buf *?owned b = buf.create(8);\n  if (!b)\n    return -1;\n"


def _lifetime(tmp_path, body):
    (tmp_path / "mem.h").write_text(MEM_H)
    return _warnings(_yasc(tmp_path, BUF + body, "-Wlifetime"))


def test_lifetime_clean(tmp_path):
    assert _lifetime(tmp_path, "int f(void)\n{\n" + OPEN + "  int n = b->size();\n  b->destroy();\n  return n;\n}\n") == []


def test_lifetime_leak(tmp_path):
    assert _lifetime(tmp_path, "int f(void)\n{\n" + OPEN + "  return b->size();\n}\n") == [
        "'b' leaks: it still owns memory at return"
    ]


def test_lifetime_double_drop(tmp_path):
    assert _lifetime(tmp_path, "int f(void)\n{\n" + OPEN + "  b->destroy();\n  b->destroy();\n  return 0;\n}\n") == [
        "'b' is freed twice"
    ]


def test_lifetime_use_after_drop(tmp_path):
    assert _lifetime(tmp_path, "int f(void)\n{\n" + OPEN + "  b->destroy();\n  return b->size();\n}\n") == [
        "'b' is used after it was freed"
    ]


def test_lifetime_maybe_self(tmp_path):
    assert _lifetime(
        tmp_path,
        "int f(void)\n{\n  struct buf *?owned b = buf.create(8);\n  int n = b->size();\n  mem_free(b);\n  return n;\n}\n",
    ) == ["a pointer that may be null is passed as argument 1 of 'buf__size', which is not '?'"]


def test_lifetime_stack_object_cannot_drop(tmp_path):
    """obj.m() passes &obj: a view, which an owned self cannot take."""
    assert _lifetime(tmp_path, "void f(struct buf b)\n{\n  b.destroy();\n}\n") == [
        "a pointer that owns nothing is stored in an owned pointer"
    ]


# Modules: the exported struct's methods are exported with it.
MODULE = """#pragma yasc
module pts;
export struct pt
{
  int x, y;
  struct pt make(int x, int y) { return (struct pt){x, y}; }
  int sum(const struct pt *self) { return self->x + self->y; }
  inline int neg(const struct pt *self) { return -self->x; }
  static inline int twice(const struct pt *self) { return 2 * pt__sum(self); }
};
struct hidden
{
  int v;
  int get(const struct hidden *self) { return self->v; }
};
export int pts_hidden(int v) { struct hidden h = {v}; return h.get(); }
"""


def test_module_c_header(tmp_path):
    proc = _compile(tmp_path, MODULE, "-ccompat", "-fmodule-dir=ifc", "-fmodule-header=include", name="pts.c")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    header = (tmp_path / "include" / "pts.h").read_text()
    body = header.split("#define YASC_MODULE_PTS_H", 1)[1]
    # the struct without its methods, then each method's C declaration
    assert "struct pt {\n  int x, y;\n};" in body, header
    for decl in ("struct pt pt__make(int x, int y);", "int pt__sum(const struct pt * self);",
                 "int pt__neg(const struct pt * self);", "static inline int pt__twice(const struct pt * self) {"):
        assert decl in body, header
    assert "hidden" not in body.replace("pts_hidden", ""), header
    # a C program uses them through the header
    proc = _compile(tmp_path, '#include "pts.h"\nint f(void) { struct pt p = pt__make(1, 2); return pt__sum(&p) + pt__twice(&p); }\n',
                    "-ccompat", "-Iinclude", name="c_user.c")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_module_importer(tmp_path):
    proc = _compile(tmp_path, MODULE, "-ccompat", "-fmodule-dir=ifc", name="pts.c")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    proc = _yasc(tmp_path, "import pts;\nint f(void) { struct pt p = pt.make(1, 2); return p.sum() + p.neg() + p.twice(); }\n",
                 "-fmodule-dir=ifc", "-O1")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    # a private struct's methods are not in the interface
    proc = _yasc(tmp_path, "import pts;\nint f(void) { struct hidden h = {1}; return h.get(); }\n", "-fmodule-dir=ifc")
    assert proc.returncode != 0


def test_module_header_rejects_method_call_in_inline(tmp_path):
    src = MODULE.replace("2 * pt__sum(self)", "2 * self->sum()")
    proc = _compile(tmp_path, src, "-ccompat", "-fmodule-dir=ifc", "-fmodule-header=include", name="pts.c")
    assert _error(proc) == (
        "-fmodule-header: an exported static inline function calls the method 'sum': "
        "C has no method calls, call the function TAG__sum"
    )
    # without a C header the body is fine
    proc = _compile(tmp_path, src, "-ccompat", "-fmodule-dir=ifc", name="pts.c")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
