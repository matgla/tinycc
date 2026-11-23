"""ARM stack frames of the expression parser's recursive path, as the device
tcc is built (the unity TU, -O2, the native release defines).

Nested parentheses recurse unary() -> unary_paren() -> gexpr() -> expr_eq()
-> expr_cond() -> expr_unary_infix() -> unary(); a binary operand adds
expr_infix_l(), a subscript unary_subscript().  The device tcc has a 32 KiB
stack: at c35ae0af90 that cycle took 936 bytes (unary() alone 464, it was 72
at 471ab721f5) and fuzz tests with ~15 nested casts overflowed it.  The host
proxy of the same check is tests/ir_tests/test_parse_stack_depth.py.
"""

import re
import subprocess
from pathlib import Path

import pytest

from test_selfhost_compile import _probe_compiler, _selfhost_defines, _selfhost_include_dirs

# The paren cycle was 344 bytes at 471ab721f5, 936 at c35ae0af90, 132 since.
CYCLES = {
    "paren": (["unary", "unary_paren", "gexpr", "expr_eq", "expr_cond", "expr_unary_infix"], 256),
    "operand": (["expr_infix_l", "unary", "unary_paren", "gexpr", "expr_eq", "expr_cond", "expr_unary_infix"], 288),
    "cast": (["unary", "unary_paren"], 128),
    "subscript": (["unary", "unary_subscript", "gexpr", "expr_eq", "expr_cond", "expr_unary_infix"], 288),
}


@pytest.fixture(scope="module")
def frames(selfhost_compiler, tmp_path_factory):
    root = Path(__file__).parent.parent.parent
    tmp = tmp_path_factory.mktemp("frames")
    inc = _selfhost_include_dirs(root)
    defines = _selfhost_defines("release")
    ok, out = _probe_compiler(selfhost_compiler, inc, defines, tmp)
    if not ok:
        pytest.skip(f"cross compiler cannot parse tcc.h here:\n{out}")
    cmd = [str(selfhost_compiler), "-ccompat", "-O2", "-c", "-fstack-usage", "-mcpu=cortex-m33", "-ffunction-sections"]
    for d in inc:
        cmd += ["-I", str(d)]
    cmd += [f"-D{d}" for d in defines]
    obj = tmp / "frontend_gen_expr.o"
    cmd += [str(root / "source/unity/frontend_gen_expr.c"), "-o", str(obj)]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=tmp)
    assert proc.returncode == 0, proc.stderr
    su = obj.with_suffix(".su").read_text()
    return {m.group(1): int(m.group(2)) for m in re.finditer(r":(\w+)\t(\d+)\t", su)}


@pytest.mark.selfhost
@pytest.mark.selfhost_compile
@pytest.mark.parametrize("cycle", sorted(CYCLES))
def test_parser_recursion_frames(frames, cycle):
    funcs, budget = CYCLES[cycle]
    missing = [f for f in funcs if f not in frames]
    assert not missing, f"not in the .su output: {missing}"
    total = sum(frames[f] for f in funcs)
    detail = ", ".join(f"{f} {frames[f]}" for f in funcs)
    assert total <= budget, (
        f"the {cycle} recursion takes {total} bytes of ARM stack a level (budget {budget}): {detail}. "
        "Move the cold, local-heavy part out of line (noinline), as expr/unary.c does."
    )
