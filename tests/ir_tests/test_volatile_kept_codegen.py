"""Volatile accesses that reach the machine code exactly as often as written.

Each case is a QEMU-runnable bug_*.c program; the counts here cover the
accesses a run cannot observe (frame slots, ordinary memory).
"""
import pytest

from test_codegen_asm import ROOT, _compile, _disassemble, _mem_counts

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

# vstore's delayed char/short cast dropped VT_VOLATILE from the unloaded
# source, so a dead narrow local lost its volatile load.  function -> loads.
NARROW_LOAD_WANT = {
    "w_short": 1, "w_char": 1, "w_schar": 1, "w_assign": 1, "w_array": 1,
    "w_bool": 1, "w_int": 1, "h_short": 1, "b_uchar": 1, "h_uchar": 1,
    "d_short": 1, "h_twice": 2, "v_value": 2,
    # Non-volatile neighbours: the dead load still goes.
    "p_short": 0, "p_char": 0,
}


@pytest.mark.parametrize("opt", LEVELS)
def test_narrow_volatile_load_dead_local(opt):
    funcs = _disassemble(_compile("bug_narrow_volatile_load_dead_local", [opt], ROOT / "tests/ir_tests"))
    got = {name: _mem_counts(funcs[name])[0] for name in NARROW_LOAD_WANT}
    assert got == NARROW_LOAD_WANT, f"volatile loads at {opt}: {got}"


# vstore's 64-bit store retyped its destination to the bare basic type,
# dropping VT_VOLATILE, so ptr_store_dse deleted an overwritten volatile
# strd.  function -> (loads, stores), exact.
STORE64_WANT = {
    "s64": (0, 2), "sll": (0, 2), "sdbl": (0, 2), "sidx": (0, 2),
    "smix": (0, 3), "scopy": (2, 2),
}


@pytest.mark.parametrize("opt", LEVELS)
def test_volatile_64bit_store_kept(opt):
    funcs = _disassemble(_compile("bug_volatile_64bit_store_kept", [opt], ROOT / "tests/ir_tests"))
    got = {name: _mem_counts(funcs[name]) for name in STORE64_WANT}
    assert got == STORE64_WANT, f"volatile 64-bit accesses at {opt}: {got}"
    # The store through (volatile u64 *)&x of a plain local stays.
    assert any(m.startswith("strd") for m, _ in funcs["l64"]), funcs["l64"]
    # Non-volatile neighbour: the overwritten store still goes above -O0.
    if opt != "-O0":
        assert _mem_counts(funcs["p64"]) == (0, 1), funcs["p64"]


# redundant_var_assign deleted an overwritten def of a volatile VAR (a store
# to its slot with no lvalue operand).  function -> (loads, stores), exact.
REDUNDANT_ASSIGN_WANT = {
    "v_twice": (2, 3), "v_char": (2, 3), "v_u64": (3, 6),
    # Non-volatile neighbour: no frame traffic at all.
    "p_twice": (0, 0),
}


@pytest.mark.parametrize("opt", LEVELS)
def test_redundant_assign_volatile_local(opt):
    funcs = _disassemble(_compile("bug_redundant_assign_volatile_local", [opt], ROOT / "tests/ir_tests"))
    got = {name: _mem_counts(funcs[name]) for name in REDUNDANT_ASSIGN_WANT}
    assert got == REDUNDANT_ASSIGN_WANT, f"volatile local accesses at {opt}: {got}"
