"""Writing `inline` must not make a function less likely to be inlined.

A plain static function is compiled when it is defined, and what it became decides
whether its callers expand it.  A static inline is only saved, and compiled after
every call site has decided, so all it had was the length of its token stream: casts,
parentheses and line numbers included.  Accessors with an early return stayed calls
when written `static inline`, and were expanded when written `static`.
"""

import subprocess

import pytest

from conftest import _find_compiler

LEVELS = ["-O1", "-O2"]

PRELUDE = """
typedef unsigned int u32;
typedef int s32;
typedef struct Op { s32 word; s32 imm; } Op;
typedef struct Quad { unsigned short base; unsigned char op; } Quad;
typedef struct State { Op *pool; int count; } State;
extern const struct { unsigned char has_dest, has_src1; } config[];
"""

# Each: its definition, then callers that should end up without a call.
EARLY_RETURNS = (
    """
{storage} int get_vreg(int w)
{{
  const u32 type = (u32)w >> 29, position = (u32)w & 0x7FFF;
  if (type == 0)
    return -1;
  if (type == 7)
  {{
    if (w == -1)
      return -1;
    if ((position & 0x7FF0) == 0x7FF0)
      return -(int)((position & 0xF) + 1);
  }}
  return (int)((type << 29) | position);
}}
""",
    "int a(int w) { return get_vreg(w) + 1; } int b(int w, int x) { return get_vreg(w) + get_vreg(x); }",
)

STRUCT_RESULT = (
    """
{storage} Op get_src1(const State *s, const Quad *q)
{{
  if (!config[q->op].has_src1)
    return (Op){{.word = (s32)-1, .imm = (s32)0}};
  int off = config[q->op].has_dest;
  if ((int)q->base + off >= s->count)
    return (Op){{.word = (s32)-1, .imm = (s32)0}};
  return s->pool[q->base + off];
}}
""",
    "int a(const State *s, const Quad *q) { return get_src1(s, q).word; } "
    "int b(const State *s, const Quad *q) { return get_src1(s, q).imm + get_src1(s, q + 1).word; }",
)


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


def build(tcc, tmp_path, source, level):
    src, obj = tmp_path / "t.c", tmp_path / "t.o"
    src.write_text(PRELUDE + source)
    r = subprocess.run([tcc, level, "-nostdlib", "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    dis = subprocess.run(["arm-none-eabi-objdump", "-dr", str(obj)], capture_output=True, text=True, check=True).stdout
    syms = subprocess.run(["arm-none-eabi-nm", str(obj)], capture_output=True, text=True, check=True).stdout
    calls = [l.split()[-1] for l in dis.splitlines() if "R_ARM_THM_CALL" in l or "R_ARM_THM_JUMP24" in l]
    return calls, [l.split()[-1] for l in syms.splitlines() if l.split()[-2] in "tT"]


@pytest.mark.parametrize("level", LEVELS)
@pytest.mark.parametrize("definition,callers", [EARLY_RETURNS, STRUCT_RESULT], ids=["early_returns", "struct_result"])
def test_static_inline_expands_as_plain_static_does(tcc, tmp_path, level, definition, callers):
    plain, _ = build(tcc, tmp_path, definition.format(storage="static") + callers, level)
    with_inline, funcs = build(tcc, tmp_path, definition.format(storage="static inline") + callers, level)
    assert plain == []
    assert with_inline == []
    assert funcs == ["a", "b"]


@pytest.mark.parametrize("level", LEVELS)
def test_source_layout_does_not_decide(tcc, tmp_path, level):
    # Every token saved on a new line costs the token stream a line number.
    expression = "x + 1 + ( x * 2 ) + ( x ^ 3 ) + ( x & 4 ) + ( x | 5 ) + ( x - 6 ) + ( x >> 2 ) + ( x << 3 )"
    caller = "int a(int x) { return f(x) + f(x + 1); }"
    one_line = f"static inline int f(int x) {{ return {expression}; }}\n"
    many = "static inline int f(int x)\n{\n  return\n" + expression.replace(" ", "\n") + "\n;\n}\n"
    assert build(tcc, tmp_path, one_line + caller, level)[0] == []
    assert build(tcc, tmp_path, many + caller, level)[0] == []


# What must stay as it was.


@pytest.mark.parametrize("level", LEVELS)
def test_large_static_inline_stays_a_call(tcc, tmp_path, level):
    ops = "".join(f"  x = (x ^ {k + 3}) * (x + {k + 7});\n" for k in range(40))
    calls, funcs = build(
        tcc, tmp_path, f"static inline int big(int x)\n{{\n{ops}  return x;\n}}\nint a(int x) {{ return big(x) + big(x + 1); }}", level
    )
    assert calls.count("big") == 2
    assert "big" in funcs


@pytest.mark.parametrize("level", LEVELS)
def test_static_inline_with_a_loop_stays_a_call(tcc, tmp_path, level):
    loop = "".join(f"    s += p[i + {k}] ^ {k + 3};\n" for k in range(12))
    calls, _ = build(
        tcc,
        tmp_path,
        f"static inline int sum(const int *p, int n)\n{{\n  int s = 0;\n  for (int i = 0; i < n; i++)\n  {{\n{loop}  }}\n  return s;\n}}\n"
        "int a(const int *p) { return sum(p, 3) + sum(p + 1, 4); }",
        level,
    )
    assert calls.count("sum") == 2


@pytest.mark.parametrize("level", LEVELS)
def test_static_inline_keeps_its_body_when_its_address_is_taken(tcc, tmp_path, level):
    definition = EARLY_RETURNS[0].format(storage="static inline")
    _, funcs = build(tcc, tmp_path, definition + "int (*fp(void))(int) { return get_vreg; }" + EARLY_RETURNS[1], level)
    assert "get_vreg" in funcs


@pytest.mark.parametrize("level", LEVELS)
def test_unused_static_inline_emits_nothing(tcc, tmp_path, level):
    definition = EARLY_RETURNS[0].format(storage="static inline")
    _, funcs = build(tcc, tmp_path, definition + "int a(int x) { return x; }", level)
    assert funcs == ["a"]
