"""The expression parser's stack per nesting level.

unary() -> unary_paren() -> gexpr() -> expr_eq() -> expr_cond() ->
expr_unary_infix() -> unary() is one cycle per nested parenthesis, a cast
adds unary() -> unary_paren() -> unary(), and a subscript unary_subscript()
-> gexpr().  The device tcc runs with a 32 KiB stack: when the member access,
subscript, assignment and diagnostic bookkeeping sat in these frames (unary()
alone 112 -> 544 bytes on x86, 72 -> 464 on ARM), fuzz tests with 15 levels
of `(unsigned)(...)` and gcc torture compile/20030216-1.c overflowed it.  The
cold parts are out of line now (expr/unary.c, expr/infix.c).

Here: the host cross under a fixed RLIMIT_STACK compiles 200 nested levels of
each shape.  Each limit is what the 471ab721f5 cross (before the frames grew)
needs plus a little; the grown frames needed 1.5-3x that, the fixed ones need
about half.  Measured on the gcc -O2 build of the cross.  The ARM frames of
the same functions (what the device runs) are checked in
tests/selfhost/test_parser_stack_frames.py.
"""

import resource
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
ROOT = CURRENT_DIR.parent.parent
TCC = ROOT / "armv8m-tcc"


def _host_build_plain():
    """The limits are for an optimised build without sanitizers."""
    mak = ROOT / "config.mak"
    if not mak.exists():
        return True
    text = mak.read_text(errors="replace")
    return "-fsanitize" not in text


pytestmark = [
    pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc"),
    pytest.mark.skipif(not _host_build_plain(), reason="sanitizer build: frames differ"),
]

DEPTH = 200

HEAD = """struct S { int a; int b[4]; struct S *p; };
int f(struct S *s, int x)
{
  return """

SHAPES = {
    # `((x + 1) + 1)`: the plain cycle
    "paren": ["(%s + 1)"],
    # `(int)((int)(x))`: a cast is unary_paren() twice and unary() twice
    "cast": ["(int)(%s)"],
    # member access and a subscript around every level
    "member": ["(s->p->b[(%s) & 3] + s->a)"],
    # all of them, and a conditional
    "mix": ["(%s + 1)", "(unsigned)(%s)", "s->p->b[(%s) & 3]", "(s->a ? (%s) : s->b[1])"],
}

# KiB of stack: 471ab721f5 needed (paren 100, cast 164, member 216, mix 168),
# c35ae0af90 needed 244, >400, >400, >400 (-Wall: mix >400); the fixed parser
# 44, 68, 100, 92 (-Wall: mix 104).
LIMITS = {"paren": 128, "cast": 192, "member": 256, "mix": 192}


def _source(shape):
    forms = SHAPES[shape]
    e = "x"
    for i in range(DEPTH):
        e = forms[i % len(forms)] % e
    return HEAD + e + ";\n}\n"


def _compile_limited(tmp_path, shape, kib, *flags):
    src = tmp_path / f"{shape}.c"
    src.write_text(_source(shape))

    def limit():
        resource.setrlimit(resource.RLIMIT_STACK, (kib * 1024, kib * 1024))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    return subprocess.run(
        [str(TCC), "-ccompat", "-nostdlib", "-c", *flags, str(src), "-o", str(tmp_path / f"{shape}.o")],
        capture_output=True,
        text=True,
        preexec_fn=limit,
    )


@pytest.mark.parametrize("opt", ["-O0", "-O1"])
@pytest.mark.parametrize("shape", sorted(SHAPES))
def test_nested_expression_fits_stack(tmp_path, shape, opt):
    proc = _compile_limited(tmp_path, shape, LIMITS[shape], opt)
    assert proc.returncode == 0, (
        f"{DEPTH} nested levels ({shape}) did not compile in {LIMITS[shape]} KiB of stack "
        f"(rc {proc.returncode}): a parser frame on the recursive path grew\n{proc.stderr[-2000:]}"
    )


def test_nested_expression_fits_stack_wall(tmp_path):
    """-Wall turns the operand hash on (-Wtautological-compare): its variants
    of expr_unary_infix()/expr_infix_l() keep more state per level."""
    proc = _compile_limited(tmp_path, "mix", LIMITS["mix"], "-O1", "-Wall")
    assert proc.returncode == 0, proc.stderr[-2000:]


# Nested inline expansions: at -O1/-O2 the frontend expands a small static
# function's body at each call, replaying its tokens inside the caller's
# parse, and an expansion's own calls expand in turn, up to INLINE_NEST_MAX
# (32) deep -- Zig's C (called-once chains) gets there.  Each level is
# unary_funcall_body() -> replay_body_recovering() -> recover_call() ->
# replay_block() -> block() -> block() -> ... -> unary() -> unary_funcall(),
# plus decl() and the initializer when the call initializes a local.  At
# c35ae0af90 the error recovery's jmp_buf and saved state (replay_recovering),
# the call's -Wformat / -Wsizeof-pointer-memaccess state and the statement
# attributes' AttributeDef sat in those frames: the device tcc ran out of its
# 32 KiB compiling the Zig-rendered YasOS kernel.
INLINE_DEPTH = 40

INLINE_SHAPES = {
    # `return f(x) + 1;`
    "ret": "static inline int f%d(int x) { return f%d(x) + 1; }",
    # a local initialized by the call: decl() and the initializer too
    "decl": "static inline int f%d(int x) { int y = f%d(x + 1); return y * 2; }",
    # an if and a compound statement around it
    "if": "static inline int f%d(int x) { if (x) { int y = f%d(x - 1); return y; } return 0; }",
}

# KiB: 471ab721f5 needed (ret 74, decl 112, if 128), c35ae0af90 112, 158,
# 178; the fixed parser 71, 117, 136 (-O1 or -O2, whichever needs more).
INLINE_LIMITS = {"ret": 96, "decl": 136, "if": 160}


def _inline_source(shape):
    lines = ["static inline int f%d(int x) { return x + 1; }" % INLINE_DEPTH]
    for i in range(INLINE_DEPTH - 1, -1, -1):
        lines.append(INLINE_SHAPES[shape] % (i, i + 1))
    lines.append("int top(int x) { return f0(x); }")
    return "\n".join(lines) + "\n"


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
@pytest.mark.parametrize("shape", sorted(INLINE_SHAPES))
def test_nested_inline_expansion_fits_stack(tmp_path, shape, opt):
    src = tmp_path / f"inline_{shape}.c"
    src.write_text(_inline_source(shape))
    kib = INLINE_LIMITS[shape]

    def limit():
        resource.setrlimit(resource.RLIMIT_STACK, (kib * 1024, kib * 1024))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    proc = subprocess.run(
        [str(TCC), "-ccompat", "-nostdlib", "-c", opt, str(src), "-o", str(tmp_path / f"inline_{shape}.o")],
        capture_output=True,
        text=True,
        preexec_fn=limit,
    )
    assert proc.returncode == 0, (
        f"{INLINE_DEPTH} nested inline expansions ({shape}) did not compile in {kib} KiB of stack "
        f"(rc {proc.returncode}): a frame on the expansion's recursive path grew\n{proc.stderr[-2000:]}"
    )
