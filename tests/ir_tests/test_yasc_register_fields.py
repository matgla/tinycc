"""yasc register fields: `ro`, `wo`, `w1c` after a member's declarator.

    typedef union {
      uint32_t raw;
      struct { uint32_t en:1, irq:1 w1c, _r:30; } b;
    } ctrl_t;
    struct regs { volatile uint32_t status w1c; volatile uint32_t id ro; volatile uint32_t tx wo; };

- writing an `ro` field of volatile memory, or reading a `wo` one, is an
  error (frontend);
- a store to a register whose w1c bits still hold what a read of it gave
  -- `v = *r; v.b.en = 1; *r = v;`, `regs->status |= X`, `r->raw |= 1` --
  is an error (ir/regaccess.c): those bits read as 1 are written back and
  clear the pending flag.  AND / OR with a constant make bits known, XOR
  with one flips them; a shift moves every bit away from where it was read
  and arithmetic makes a new value; a branch that sets them on one path
  only does not.
-Wregister-access, strict; unsafe lowers it; the same at every -O.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

PRELUDE = """\
typedef unsigned uint32_t;
typedef union { uint32_t raw; struct { uint32_t en:1, irq:1 w1c, _r:6, div:8, _r2:16; } b; } ctrl_t;
typedef struct { uint32_t ctrl; uint32_t status w1c; } pair_t;
typedef struct { uint32_t a, b, c; uint32_t st w1c; uint32_t e; } big_t;
ctrl_t read_ctrl(volatile ctrl_t *r);
void read_into(volatile ctrl_t *r, inout ctrl_t *dst);
void build(inout ctrl_t *dst);
struct regs {
  volatile uint32_t ctrl;
  volatile uint32_t status w1c;
  volatile uint32_t id ro;
  volatile uint32_t txdata wo;
};
"""
PRELUDE_LINES = PRELUDE.count("\n")

W1C = "w1c field '%s' is written back as it was read, which clears it: set it to 0 to keep it (1 clears it)"


def _compile(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
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


BAD = {
    "copy_written_back": (
        "void f(inout volatile ctrl_t *r)\n{\n  ctrl_t v = *r;\n  v.b.en = 1;\n  *r = v;\n}\n", 5, W1C % "irq"),
    "set_on_one_path": (
        "void f(inout volatile ctrl_t *r, int x)\n{\n  ctrl_t v = *r;\n  if (x)\n    v.b.irq = 0;\n  *r = v;\n}\n",
        6,
        W1C % "irq",
    ),
    "or_assign": ("void f(inout struct regs *g)\n{\n  g->status |= 4;\n}\n", 3, W1C % "status"),
    "echo_through_local": (
        "void f(inout struct regs *g)\n{\n  uint32_t s = g->status;\n  g->status = s;\n}\n", 4, W1C % "status"),
    "raw_member_of_union": ("void f(inout volatile ctrl_t *r)\n{\n  r->raw |= 1;\n}\n", 3, W1C % "irq"),
    "call_result": (
        "void f(inout volatile ctrl_t *r)\n{\n  ctrl_t v = read_ctrl(r);\n  v.b.en = 1;\n  *r = v;\n}\n", 5, W1C % "irq"),
    "filled_by_call": (
        "void f(inout volatile ctrl_t *r)\n{\n  ctrl_t v = undefined;\n  read_into(r, &v);\n  v.b.en = 1;\n  *r = v;\n}\n",
        6,
        W1C % "irq",
    ),
    "second_word": (
        "void f(inout volatile pair_t *r)\n{\n  pair_t v = *r;\n  v.ctrl = 1;\n  *r = v;\n}\n", 5, W1C % "status"),
    "through_memmove": (
        "void f(inout volatile big_t *r)\n{\n  big_t v = *r;\n  v.a = 1;\n  *r = v;\n}\n", 5, W1C % "st"),
    "xor_constant": (
        "void f(inout struct regs *g)\n{\n  uint32_t s = g->status;\n  g->status = s ^ 1;\n}\n", 4, W1C % "status"),
    "and_of_two_reads": (
        "void f(inout struct regs *g)\n{\n  uint32_t s = g->status;\n  uint32_t m = g->ctrl;\n  g->status = s & m;\n}\n",
        5,
        W1C % "status",
    ),
    "echo_in_one_statement": ("void f(inout struct regs *g)\n{\n  g->status = g->status;\n}\n", 3, W1C % "status"),
    "and_of_two_reads_in_one_statement": (
        "struct uart { volatile uint32_t ris ro; volatile uint32_t imsc; volatile uint32_t icr w1c; };\n"
        "void f(inout struct uart *u)\n{\n  u->icr = u->ris & u->imsc;\n}\n",
        4,
        W1C % "icr",
    ),
    "or_of_a_read_in_one_statement": ("void f(inout struct regs *g)\n{\n  g->status = g->status | g->ctrl;\n}\n", 3,
                                      W1C % "status"),
    "zero_shift": (
        "void f(inout struct regs *g)\n{\n  uint32_t s = g->status;\n  g->status = s << 0;\n}\n", 4, W1C % "status"),
    "ro_written": ("void f(inout struct regs *g)\n{\n  g->id = 1;\n}\n", 3,
                   "'id' is a read-only register field: writing it does nothing, or worse"),
    "wo_read": ("uint32_t f(struct regs *g)\n{\n  return g->txdata;\n}\n", 3,
                "'txdata' is a write-only register field: reading it does not give what was written"),
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(BAD))
def test_register_misuse(tmp_path, name, opt):
    body, line, msg = BAD[name]
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode != 0
    assert (line, "error", msg) in _diags(proc), proc.stderr
    # -Wno-register-access cannot lower it
    low = _compile(tmp_path, body, opt, "-Wno-register-access")
    assert (line, "error", msg) in _diags(low), low.stderr


GOOD = {
    "keep_pending": "void f(inout volatile ctrl_t *r)\n{\n  ctrl_t v = *r;\n  v.b.en = 1;\n  v.b.irq = 0;\n  *r = v;\n}\n",
    "clear_on_purpose": "void f(inout volatile ctrl_t *r)\n{\n  ctrl_t v = *r;\n  v.b.irq = 1;\n  *r = v;\n}\n",
    "masked": "void f(inout volatile ctrl_t *r)\n{\n  r->raw = (r->raw | 1) & ~2u;\n}\n",
    "ack_by_value": "void f(inout struct regs *g)\n{\n  g->status = 4;\n}\n",
    # the w1c store of one statement is not the next one's
    "next_statement_reads": (
        "void f(inout struct regs *g)\n{\n  g->status = 4;\n  g->ctrl = g->ctrl | 1;\n}\n"
    ),
    "ro_read": "uint32_t f(struct regs *g)\n{\n  return g->id;\n}\n",
    "wo_written": "void f(inout struct regs *g, uint32_t c)\n{\n  g->txdata = c;\n}\n",
    "ctrl_rmw": "void f(inout struct regs *g)\n{\n  g->ctrl |= 1;\n}\n",
    "image_not_volatile": "void f(inout ctrl_t *img)\n{\n  img->raw |= 1;\n}\n",
    "filled_then_kept": (
        "void f(inout volatile ctrl_t *r)\n{\n  ctrl_t v = undefined;\n  read_into(r, &v);\n  v.b.irq = 0;\n  *r = v;\n}\n"
    ),
    "built_without_the_register": "void f(inout volatile ctrl_t *r)\n{\n  ctrl_t v = undefined;\n  build(&v);\n  *r = v;\n}\n",
    "second_word_kept": (
        "void f(inout volatile pair_t *r)\n{\n  pair_t v = *r;\n  v.ctrl = 1;\n  v.status = 0;\n  *r = v;\n}\n"
    ),
    "memmove_kept": "void f(inout volatile big_t *r)\n{\n  big_t v = *r;\n  v.a = 1;\n  v.st = 0;\n  *r = v;\n}\n",
    "fresh_value": "void f(inout volatile big_t *r)\n{\n  big_t v = {1, 2, 3, 0, 5};\n  *r = v;\n}\n",
    # a bit of another register moved to bit 0: not what this one read
    "shifted_bit": "void f(inout struct regs *g)\n{\n  uint32_t p = g->ctrl;\n  g->status = (p >> 4) & 1;\n}\n",
    "shifted_left": "void f(inout struct regs *g)\n{\n  uint32_t p = g->ctrl;\n  g->status = p << 2;\n}\n",
    "arithmetic": "void f(inout struct regs *g)\n{\n  uint32_t p = g->ctrl;\n  g->status = p + 1;\n}\n",
    "unsafe": "void f(inout struct regs *g)\n{\n  unsafe {\n    g->status |= 4;\n  }\n}\n",
    "address_of_wo": "volatile uint32_t * view f(inout struct regs *g)\n{\n  return &g->txdata;\n}\n",
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("name", sorted(GOOD))
def test_register_use(tmp_path, name, opt):
    proc = _compile(tmp_path, GOOD[name], opt)
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


def test_one_qualifier_per_field(tmp_path):
    proc = _compile(tmp_path, "struct r2 { volatile uint32_t x ro wo; };\n")
    assert (1, "error", "a register field is one of ro, wo or w1c") in _diags(proc), proc.stderr


def test_regular_c_names_stay_names(tmp_path):
    """ro, wo and w1c are only qualifiers after a member's declarator: as
    names they still work."""
    proc = _compile(tmp_path, "struct r3 { int ro, wo, w1c; };\nint ro(int wo) { int w1c = wo; return w1c; }\n")
    assert proc.returncode == 0 and _diags(proc) == [], proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2", "-fsyntax-only"])
def test_notes_freed_when_the_body_has_an_error(tmp_path, opt):
    """A body with an error skips the flow checks: its w1c stores, calls
    handed volatile memory and tagged-union accesses are still freed (an
    ASan build reports a leak otherwise)."""
    body = (
        "enum vk { V_INT, V_STR };\n"
        "struct value { enum vk k; union(k) { long i when(V_INT); char * view s when(V_STR); } u; };\n"
        "long f(inout struct regs *g, volatile ctrl_t *r, struct value *v)\n{\n"
        "  g->status = 4;\n  ctrl_t c = read_ctrl(r);\n  long x = v->u.i;\n  return nope + c.raw + x;\n}\n"
    )
    proc = _compile(tmp_path, body, opt)
    assert proc.returncode == 1, proc.stderr
    assert "'nope' undeclared" in proc.stderr, proc.stderr
    assert "Sanitizer" not in proc.stderr, proc.stderr
