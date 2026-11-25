"""-fyasc-facts (S4, docs/static_analysis_todo.md): the optimizer may assume
what checked yasc types say.

O1, a `view` parameter does not escape: a frame object whose address only
goes to memory accesses, constant offsets and such parameters is reached by
no later call (sl_forward forwards its stores across them); a call it is
handed to may still write it unless the parameter points to const.  The
facts are taken only in the safe default (strict lifetimes), at -O1+.
A named scalar whose address only goes to such parameters (`int x;
get(&x);`) gets a register; its slot is used only around those calls.

The run test compiles yasc_facts_cases.c in the safe default with and
without the facts, links each with yasc_facts_main.c (regular C, the
callees) and runs both on QEMU: they must print the same as -O0.
"""

import os
import subprocess
from pathlib import Path

import pytest

from qemu_run import CompileConfig, build_qemu_command, compile_testcase

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"
MACHINE = "mps2-an505"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

OBJ_FLAGS = ["-nostdlib", "-fvisibility=hidden", "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft",
             "-ffunction-sections"]


def _cases_obj(tmp_path, opt, facts, name):
    (tmp_path / "objs").mkdir(exist_ok=True)  # the harness cleans its own output directory
    obj = tmp_path / "objs" / f"{name}.o"
    flags = [opt] + (["-fyasc-facts"] if facts else [])
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-Wno-cross-tu", "-Wno-tainted-format", *OBJ_FLAGS, *flags, "-c", str(CURRENT_DIR / "yasc_facts_cases.c"), "-o",
                           str(obj)], capture_output=True, text=True, env=dict(os.environ, TCC_YF_STATS="1"))
    assert proc.returncode == 0, proc.stderr
    return obj, proc.stderr


def _run(tmp_path, opt, facts):
    name = f"cases{opt[1:]}{'_f' if facts else ''}"
    obj, stats = _cases_obj(tmp_path, opt, facts, name)
    cfg = CompileConfig(extra_cflags="-O0", output_dir=tmp_path / name, output_suffix=name, extra_objs=[str(obj)])
    res = compile_testcase([CURRENT_DIR / "yasc_facts_main.c"], MACHINE, config=cfg)
    assert res.success, res.error
    proc = subprocess.run(build_qemu_command(MACHINE, res.elf_file), shell=True, capture_output=True, text=True,
                          timeout=60)
    return proc.stdout, stats


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_same_output_with_facts(tmp_path, opt):
    want, _ = _run(tmp_path, "-O0", False)
    assert "said" in want, want
    plain, _ = _run(tmp_path, opt, False)
    facts, stats = _run(tmp_path, opt, True)
    assert plain == want
    assert facts == want
    # and the facts were used
    assert "yasc-facts: sl_forward" in stats, stats


def test_facts_change_code(tmp_path):
    a, _ = _cases_obj(tmp_path, "-O2", False, "plain")
    b, _ = _cases_obj(tmp_path, "-O2", True, "facts")
    assert a.read_bytes() != b.read_bytes()


def test_off_without_the_flag_or_strict_mode(tmp_path):
    """No -fyasc-facts, -O0, or -ccompat (no strict lifetimes): nothing is
    assumed, objects are byte-identical."""
    src = CURRENT_DIR / "yasc_facts_cases.c"

    def obj(name, *flags):
        out = tmp_path / f"{name}.o"
        proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-Wno-cross-tu", "-Wno-tainted-format", *OBJ_FLAGS, *flags, "-c", str(src), "-o", str(out)], capture_output=True,
                              text=True, env=dict(os.environ, TCC_YF_STATS="1"))
        assert proc.returncode == 0, proc.stderr
        assert "yasc-facts" not in proc.stderr, proc.stderr
        return out.read_bytes()

    assert obj("o0", "-O0", "-fyasc-facts") == obj("o0p", "-O0")
    assert obj("cc", "-O2", "-fyasc-facts", "-ccompat", "-std=yasc") == obj("ccp", "-O2", "-ccompat", "-std=yasc")
