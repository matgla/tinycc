"""Golden-IR snapshot tests for TCC optimization passes.

This runner compiles a small C file with a debug-enabled TCC and compares the
`=== AFTER <pass> ===` IR block against a checked-in `.expected` file.

Usage:
    pytest tests/ir_tests/test_golden_ir.py
    pytest tests/ir_tests/test_golden_ir.py --update          # regenerate .expected
    pytest tests/ir_tests/test_golden_ir.py -k ssa_fold       # single pass case

The test tree is rooted at tests/ir_tests/golden/<pass>/<case>.c with a
matching <case>.expected file.  Pass names are the exact strings emitted by
TCC's `-dump-ir-passes=` machinery (e.g. `block_copy_init`, `ssa:fold`).
"""

import difflib
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TINYCC_DIR = CURRENT_DIR / "../.."

# Debug compiler preference order.  The task originally asked for a host `tcc`
# built with CONFIG_TCC_DEBUG, but this YasOS fork has no x86 target sources,
# so the practical debug compiler is the armv8m cross-compiler rebuilt with
# --debug.  The runner tolerates either.
DEBUG_COMPILER_CANDIDATES = [
    TINYCC_DIR / "armv8m-tcc.debug",
    TINYCC_DIR / "armv8m-tcc",
    TINYCC_DIR / "tcc",
]

# SSA passes targeted by Phase C of plan_optimizer_test_coverage.md.
# These are the names used in `ir/opt/ssa_opt.c:tcc_ir_ssa_opt_run` for
# `dbg_scan_imm_dest`, which are *not* the same as the legacy pipeline pass
# names understood by `-dump-ir-passes=`.
SSA_PASS_NAMES = {
    "ssa:branch",
    "ssa:fold",
    "ssa:sccp",
    "ssa:cprop",
    "ssa:gvn",
    "ssa:load_cse",
    "ssa:narrow",
}

GOLDEN_ROOT = CURRENT_DIR / "golden"


def _find_debug_compiler(compiler_override=None):
    if compiler_override is not None:
        p = Path(compiler_override)
        if not p.exists():
            raise FileNotFoundError(f"--compiler not found: {p}")
        return p
    for cand in DEBUG_COMPILER_CANDIDATES:
        if cand.exists():
            return cand
    raise FileNotFoundError(
        "No debug-enabled TCC found. Build one with CONFIG_TCC_DEBUG "
        "(e.g. ./configure --debug && make armv8m-tcc in libs/tinycc)."
    )


def _compiler_has_dump_ir_passes(compiler):
    """Sanity-check that the compiler supports -dump-ir-passes."""
    result = subprocess.run(
        [str(compiler), "-dump-ir-passes=all", "-c", "-x", "c", "-", "-o", "/dev/null"],
        input="int f(int x){return x;}",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return result.returncode == 0 and "=== AFTER" in result.stdout


def _discover_cases():
    """Return [(pass_name, case_name, c_file, expected_file), ...]."""
    cases = []
    if not GOLDEN_ROOT.exists():
        return cases
    for pass_dir in sorted(GOLDEN_ROOT.iterdir()):
        if not pass_dir.is_dir():
            continue
        pass_name = pass_dir.name
        for c_file in sorted(pass_dir.glob("*.c")):
            expected = c_file.with_suffix(".expected")
            cases.append((pass_name, c_file.stem, c_file, expected))
    return cases


_CASES = _discover_cases()
_CASE_IDS = [f"{pass_name}/{case_name}" for pass_name, case_name, _, _ in _CASES]


def _extract_pass_block(output, pass_name):
    """Extract the `=== AFTER pass_name ===` ... `=== END AFTER pass_name ===` block."""
    lines = output.splitlines()
    start_idx = None
    end_idx = None
    start_marker = f"=== AFTER {pass_name} ==="
    end_marker = f"=== END AFTER {pass_name} ==="
    for i, line in enumerate(lines):
        if line.strip() == start_marker:
            start_idx = i
        elif line.strip() == end_marker and start_idx is not None:
            end_idx = i
            break
    if start_idx is None:
        return None
    if end_idx is None:
        end_idx = len(lines) - 1
    # Return lines strictly between the markers, stripped of trailing whitespace.
    return "\n".join(line.rstrip() for line in lines[start_idx + 1 : end_idx])


def _run_compiler(compiler, cflags, c_file, tmp_path):
    """Run the compiler and return captured stdout/stderr text."""
    out_file = tmp_path / f"{c_file.stem}.o"
    cmd = [str(compiler), *cflags, "-c", str(c_file), "-o", str(out_file)]
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return result, cmd


@pytest.fixture(scope="session")
def debug_compiler(pytestconfig):
    require_dump_ir = pytestconfig.getoption("--require-dump-ir")
    try:
        compiler = _find_debug_compiler(pytestconfig.getoption("compiler"))
    except FileNotFoundError as exc:
        if require_dump_ir:
            raise
        pytest.skip(str(exc))
    if not _compiler_has_dump_ir_passes(compiler):
        msg = (
            f"{compiler} does not support -dump-ir-passes=all. "
            "Build a CONFIG_TCC_DEBUG compiler or run `make test-golden-ir` "
            "with GOLDEN_IR_COMPILER=/path/to/debug-tcc."
        )
        if not require_dump_ir:
            pytest.skip(msg)
        raise RuntimeError(
            f"{compiler} does not support -dump-ir-passes=all. "
            "It must be built with CONFIG_TCC_DEBUG."
        )
    return compiler



@pytest.mark.parametrize("pass_name,case_name,c_file,expected_file", _CASES, ids=_CASE_IDS)
@pytest.mark.golden_ir
def test_golden_ir(pass_name, case_name, c_file, expected_file, debug_compiler, tmp_path, request):
    updating = request.config.getoption("--update")

    cflags = ["-O2", f"-dump-ir-passes={pass_name}"]
    result, cmd = _run_compiler(debug_compiler, cflags, c_file, tmp_path)

    if result.returncode != 0:
        raise AssertionError(
            f"Compilation failed for {pass_name}/{case_name}\n"
            f"Command: {' '.join(cmd)}\n"
            f"Output:\n{result.stdout}"
        )

    actual = _extract_pass_block(result.stdout, pass_name)

    if pass_name in SSA_PASS_NAMES and actual is None:
        # The SSA optimizer runs inside ir/regalloc.c and calls
        # `dbg_scan_imm_dest`, not the `dump_ir_after_pass` machinery that
        # `-dump-ir-passes=` gates.  Until the SSA pass driver is wired into
        # `RUN_PASS`, we cannot capture per-SSA-pass golden blocks this way.
        msg = (
            f"SSA pass '{pass_name}' did not emit a '=== AFTER {pass_name} ===' "
            "block. The SSA optimizer in ir/regalloc.c uses dbg_scan_imm_dest "
            "instead of the dump_ir_after_pass / RUN_PASS macro that "
            "-dump-ir-passes= controls."
        )
        if updating:
            expected_file.write_text(
                f"# AUTO-GENERATED STUB: {pass_name}/{case_name}\n"
                f"# {msg}\n"
                "# This pass cannot currently be snapshotted via -dump-ir-passes.\n"
            )
            pytest.skip(f"Wrote stub for {pass_name}/{case_name}: {msg}")
        else:
            pytest.xfail(msg)

    if actual is None:
        msg = f"Pass '{pass_name}' did not emit a '=== AFTER {pass_name} ===' block."
        if updating:
            expected_file.write_text(
                f"# AUTO-GENERATED STUB: {pass_name}/{case_name}\n# {msg}\n"
            )
            pytest.skip(f"Wrote stub for {pass_name}/{case_name}: {msg}")
        else:
            pytest.fail(msg)

    if updating:
        expected_file.parent.mkdir(parents=True, exist_ok=True)
        expected_file.write_text(actual + "\n")
        return

    if not expected_file.exists():
        pytest.fail(f"Expected file missing: {expected_file} (run with --update)")

    expected = expected_file.read_text().rstrip("\n")
    if actual != expected:
        diff = "\n".join(
            difflib.unified_diff(
                expected.splitlines(),
                actual.splitlines(),
                fromfile=str(expected_file),
                tofile=f"<actual {pass_name}/{case_name}>",
                lineterm="",
            )
        )
        raise AssertionError(
            f"Golden IR mismatch for {pass_name}/{case_name}\n\n{diff}"
        )
