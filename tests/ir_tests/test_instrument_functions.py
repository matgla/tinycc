"""-finstrument-functions: __cyg_profile_func_enter / _exit around every
function body (backend/generators/function.c, frontend/gen/stmt/block.c)."""

import subprocess
from pathlib import Path

import pytest

from qemu_run import CompileConfig, build_qemu_command, compile_testcase

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"
MACHINE = "mps2-an505"
LEVELS = ["-O0", "-O1", "-O2", "-Os"]

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")


@pytest.mark.parametrize("opt", LEVELS)
def test_exit_hook_on_every_return(tmp_path, opt):
    """bug_instrument_functions_exit.c: a function returning a value called
    the entry hook only."""
    src = "bug_instrument_functions_exit.c"
    cfg = CompileConfig(extra_cflags=f"{opt} -finstrument-functions", output_dir=tmp_path)
    res = compile_testcase([CURRENT_DIR / src], MACHINE, config=cfg)
    assert res.success, res.error
    proc = subprocess.run(build_qemu_command(MACHINE, res.elf_file), shell=True, capture_output=True, text=True,
                          timeout=60)
    want = (CURRENT_DIR / src).with_suffix(".expect").read_text().split()
    assert proc.stdout.split()[:len(want)] == want, proc.stdout + proc.stderr


def _obj(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(body)
    out = tmp_path / "t.o"
    proc = subprocess.run([str(TCC), "-ccompat", "-fno-pic", "-mno-sb-relative-got", "-c", *flags, str(src), "-o",
                           str(out)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return out.read_bytes()


@pytest.mark.parametrize("opt", LEVELS)
def test_off_means_no_hooks(tmp_path, opt):
    body = "int h(int x) { if (x) return 2; return x + 1; }\n"
    plain = _obj(tmp_path, body, opt)
    assert b"__cyg_profile" not in plain
    assert _obj(tmp_path, body, opt, "-finstrument-functions", "-fno-instrument-functions") == plain
