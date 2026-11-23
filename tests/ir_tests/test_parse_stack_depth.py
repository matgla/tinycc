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
