"""yasc unions and volatile bit-fields (S2, U1/U2), docs/static_analysis_todo.md.

A union member may have been written as another member, so what a member
that is a yasc pointer holds is not known to the checks:
- U1: a union without owned / view / borrows pointers (or handles) is not
  checked at all (registers as raw/bits, float bits);
- U2: a pointer member read through a union reads as regular C (-Wunchecked:
  owned / view / memory only in unsafe); a store to it is checked as its
  type says; its address, or an aggregate member holding such pointers used
  whole, needs unsafe; an owned member in a union is an error.
- A bit-field store to volatile memory is a hidden read-modify-write of the
  whole unit (-Wvolatile-bitfield, strict): copy, change, store back.
All are errors in the safe default and say nothing under -ccompat.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
typedef unsigned long uintptr_t;
typedef unsigned uint32_t;
int use(char *p);
union val { char * view s; uintptr_t addr; int i; };
struct holder { char * view p; int n; };
union agg { struct holder h; int raw[2]; };
union arr { char * view a[2]; uintptr_t w[2]; };
struct tagged { int k; union val u; };
typedef union { uint32_t raw; struct { uint32_t en:1, irq:1, _r:6, div:8, _r2:16; } b; } ctrl_t;
"""
PRELUDE_LINES = PRELUDE.count("\n")

FROM_C = "a pointer from regular C becomes a view here: write unsafe(...)"
DEREF = "a pointer from regular C is dereferenced: write unsafe { } around it"
WHOLE = "a union member that holds yasc pointers is used whole: write unsafe(...)"
ADDR = "the address of a union member that holds a yasc pointer is taken: write unsafe(...)"
BITFIELD = (
    "a bit-field store to volatile memory reads and writes back the whole unit: "
    "copy it to a local, change it there, store it back"
)


def _compile(tmp_path, body, *flags, prelude=PRELUDE):
    src = tmp_path / "t.c"
    src.write_text(prelude + body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-c", "-fmax-errors=0", *flags, str(src), "-o", str(tmp_path / "t.o")],
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


# one statement per line: an unsafe region owns its lines
NEEDS_UNSAFE = {
    "read": ("int f(union val *v)\n{\n  return use(%s);\n}\n", "v->s", FROM_C),
    "deref": ("int f(struct tagged *t)\n{\n  return *%s;\n}\n", "t->u.s", DEREF),
    "nested": ("int f(union agg *a)\n{\n  return use(%s);\n}\n", "a->h.p", FROM_C),
    "element": ("int f(union arr *a)\n{\n  return use(%s);\n}\n", "a->a[1]", FROM_C),
    "whole": ("struct holder f(union agg *a)\n{\n  return %s;\n}\n", "a->h", WHOLE),
    "address": ("char * view * view f(inout union val *v)\n{\n  return %s;\n}\n", "&v->s", ADDR),
}


@pytest.mark.parametrize("name", sorted(NEEDS_UNSAFE))
def test_union_pointer_read_needs_unsafe(tmp_path, name):
    fmt, expr, msg = NEEDS_UNSAFE[name]
    proc = _compile(tmp_path, fmt % expr)
    assert proc.returncode != 0
    assert (3, "error", msg) in _diags(proc), proc.stderr


@pytest.mark.parametrize("name", sorted(NEEDS_UNSAFE))
def test_union_pointer_read_in_unsafe(tmp_path, name):
    fmt, expr, _ = NEEDS_UNSAFE[name]
    proc = _compile(tmp_path, fmt % f"unsafe({expr})")
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_what_needs_no_unsafe(tmp_path):
    """Stores, scalar members, members below an aggregate that are not
    pointers, sizeof, holding the read as regular C, and unions with no yasc
    pointers at all (U1)."""
    body = (
        "#pragma tcc c_begin\nvoid c_keep(char *p);\n#pragma tcc c_end\n"
        "void f(inout union val *v, inout union agg *a, char * maybe p)\n{\n"
        "  v->addr = 0x20000000;\n"
        "  a->h.n = 3;\n"
        "  int n = v->i + a->h.n + a->raw[1] + cast<int(int)>(sizeof(v->s)) + cast<int(int)>(sizeof(a->h));\n"
        "  c_keep(v->s);\n"
        "  (void)(v->s == p);\n"
        "  (void)n;\n"
        "}\n"
        "union bits { float f; uint32_t u; };\n"
        "uint32_t g(union bits b) { return b.u; }\n"
    )
    proc = _compile(tmp_path, body)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_store_is_checked_as_its_type(tmp_path):
    """`v->s = p` stores a view: it is not regular C, so a view parameter
    stored there still cannot leave the function."""
    proc = _compile(tmp_path, "void f(inout union val *v, char *p) { v->s = p; }\n")
    assert proc.returncode != 0
    assert any(m.startswith("'p' is a view: it cannot leave the function") for _, _, m in _diags(proc)), proc.stderr


@pytest.mark.parametrize(
    "decl,what",
    [
        ("union bad { char * owned p; int x; };", " 'p'"),
        ("union bad { struct { char * owned maybe q; } s; int x; };", " 's'"),
        ("union bad { char * owned maybe a[2]; int x; };", " 'a'"),
        ("union bad { struct { char * owned r; }; int x; };", ""),
    ],
    ids=["pointer", "in_struct", "array", "anonymous_struct"],
)
def test_owned_member_in_union(tmp_path, decl, what):
    proc = _compile(tmp_path, "int close(int fd);\n" + decl + "\n")
    assert proc.returncode != 0
    msg = f"a union holds an owned member{what}: which member is live is not known, so it is never freed"
    assert (2, "error", msg) in _diags(proc), proc.stderr


BITFIELD_STORES = {
    "assign": "r->b.en = 1;",
    "compound": "r->b.div |= 2;",
    "increment": "r->b.div++;",
    "volatile_member": "s->f = 3;",
}


@pytest.mark.parametrize("name", sorted(BITFIELD_STORES))
def test_volatile_bitfield_store(tmp_path, name):
    body = (
        "struct regs { volatile uint32_t f:4; uint32_t g:4; };\n"
        "void f(inout volatile ctrl_t *r, inout struct regs *s)\n{\n  %s\n}\n" % BITFIELD_STORES[name]
    )
    proc = _compile(tmp_path, body)
    assert proc.returncode != 0
    assert (4, "error", BITFIELD) in _diags(proc), proc.stderr
    # -ccompat (regular C): nothing; -Wno-volatile-bitfield cannot lower it
    plain = PRELUDE.split("int use")[0] + PRELUDE.split("struct tagged")[1].split("\n", 1)[1] + body
    ok = _compile(tmp_path, plain.replace(" * view ", " * ").replace("inout ", ""), "-ccompat", prelude="")
    assert ok.returncode == 0 and _diags(ok) == [], ok.stderr
    low = _compile(tmp_path, body, "-Wno-volatile-bitfield")
    assert (4, "error", BITFIELD) in _diags(low), low.stderr


def test_volatile_bitfield_what_is_fine(tmp_path):
    """Reads, the explicit copy-change-store, a volatile local's initializer,
    a non-volatile register image, and unsafe."""
    body = (
        "uint32_t f(inout volatile ctrl_t *r, inout ctrl_t *img)\n{\n"
        "  ctrl_t v = *r;\n  v.b.en = 1;\n  *r = v;\n"
        "  volatile ctrl_t l = { .b = { .en = 1 } };\n  l.raw = 0;\n"
        "  img->b.div = 4;\n"
        "  unsafe {\n    r->b.irq = 1;\n  }\n"
        "  return r->b.div;\n}\n"
    )
    proc = _compile(tmp_path, body, "-Wno-unused-but-set-variable")
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr
