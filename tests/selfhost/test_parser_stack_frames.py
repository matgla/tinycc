"""ARM stack frames of the expression parser's recursive path, as the device
tcc is built (the unity TU, -O2, the native release defines).

Nested parentheses recurse unary() -> unary_paren() -> gexpr() -> expr_eq()
-> expr_cond() -> expr_unary_infix() -> unary(); a binary operand adds
expr_infix_l(), a subscript unary_subscript().  The device tcc has a 32 KiB
stack: at c35ae0af90 that cycle took 936 bytes (unary() alone 464, it was 72
at 471ab721f5) and fuzz tests with ~15 nested casts overflowed it.  The host
proxy of the same check is tests/ir_tests/test_parse_stack_depth.py.

Inline expansions recurse through the statement parser: a call whose callee
is expanded replays the body inside unary_funcall_body() --
replay_body_recovering() -> recover_call() -> replay_block() -> block()
-> block() -> gexpr() ... unary() -> unary_funcall() -> unary_funcall_body()
-- up to INLINE_NEST_MAX (32) levels, and Zig's C nests that deep.  A call
initializing a local adds decl() and the initializer.  At c35ae0af90 a level
took 2260 bytes (the recovery point's jmp_buf, the -Wformat and memaccess
states, AttributeDefs in block()), 1308 at 471ab721f5: the device tcc ran out
of stack compiling the YasOS kernel's C.  The operator and the store at the
end of every expression are checked too: they sit on top of all of it.
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
    # 2260 bytes at c35ae0af90, 1196 at ff9b93d136, 1204 since (recover_call: its kind)
    "inline": (
        [
            "unary_funcall_body",
            "replay_body_recovering",
            "recover_call",
            "replay_block",
            "block",
            "block",
            "gexpr",
            "expr_eq",
            "expr_cond",
            "expr_unary_infix",
            "unary",
            "unary_funcall",
        ],
        1280,
    ),
    # 3600 at c35ae0af90 (about 2420 at 471ab721f5), 2408 at ff9b93d136, 2416 since
    "inline_decl": (
        [
            "unary_funcall_body",
            "replay_body_recovering",
            "recover_call",
            "replay_block",
            "block",
            "decl",
            "decl_initializer_alloc",
            "decl_initializer",
            "parse_init_elem",
            "expr_eq",
            "expr_cond",
            "expr_unary_infix",
            "unary",
            "unary_funcall",
        ],
        2448,
    ),
    # the end of every expression: 416 (gen_op_checked) and 944 (vstore) at c35ae0af90
    "operator": (["gen_op_checked", "gen_op_checked_1"], 64),
    "store": (["vstore", "vstore_value_1"], 720),
}

# the TUs the functions above are in, as the native build compiles them
UNITS = [
    "source/unity/frontend_gen_expr.c",
    "source/unity/frontend_gen_builtin.c",
    "source/unity/frontend_gen_core.c",
    "source/unity/frontend_gen_stmt.c",
    "source/unity/frontend_gen_op.c",
    "source/unity/frontend_gen_store.c",
    "source/frontend/gen/decl/decl.c",
    "source/frontend/gen/init/alloc.c",
    "source/frontend/gen/init/initializer.c",
]


@pytest.fixture(scope="module")
def frames(selfhost_compiler, tmp_path_factory):
    root = Path(__file__).parent.parent.parent
    tmp = tmp_path_factory.mktemp("frames")
    inc = _selfhost_include_dirs(root)
    defines = _selfhost_defines("release")
    ok, out = _probe_compiler(selfhost_compiler, inc, defines, tmp)
    if not ok:
        pytest.skip(f"cross compiler cannot parse tcc.h here:\n{out}")
    base = [str(selfhost_compiler), "-ccompat", "-O2", "-c", "-fstack-usage", "-mcpu=cortex-m33", "-ffunction-sections"]
    for d in inc:
        base += ["-I", str(d)]
    base += [f"-D{d}" for d in defines]
    frames = {}
    for unit in UNITS:
        obj = tmp / (Path(unit).stem + ".o")
        proc = subprocess.run(base + [str(root / unit), "-o", str(obj)], capture_output=True, text=True, cwd=tmp)
        assert proc.returncode == 0, f"{unit}: {proc.stderr}"
        su = obj.with_suffix(".su").read_text()
        frames.update({m.group(1): int(m.group(2)) for m in re.finditer(r":(\w+)\t(\d+)\t", su)})
    return frames


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
