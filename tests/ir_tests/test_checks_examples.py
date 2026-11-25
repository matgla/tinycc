"""examples/checks, "Catching bugs with tinycc" (docs/checks.md): plain-C
chapters, each checked the way its README says to build it.

- 01_find_the_bugs.c is meant to warn: tcc says exactly its .diag, the same
  at -O0 and -O2.  01_fixed.c compiles clean with the same options and
  -Werror, and prints its .expect.
- 02_stack_budget.c: the stack warnings (.diag) and scripts/stack_usage.py's
  report (.stack).  Frame sizes are the generated code's, so the numbers are
  -O2's; at -O0 the same lines come out with other numbers.  It runs too.
- 03_sanitizers.c: without a sanitizer it prints its .expect without the
  "runtime error" lines; in handler mode (-fsanitize-recover, the handlers of
  ubsan_runtime.c) the reports go to stderr, in order, at their lines; in
  trap mode the first step stops with a UDF (UNDEFINSTR).
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from cross_includes import INCLUDE_FLAGS

from qemu_run import CompileConfig, build_qemu_command, compile_testcase

CURRENT_DIR = Path(__file__).parent
TOP = CURRENT_DIR.parent.parent
TCC = TOP / "armv8m-tcc"
EXAMPLES = TOP / "examples" / "checks"
MACHINE = "mps2-an505"
LEVELS = ["-O0", "-O2"]

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

# the options 01_find_the_bugs.c's header gives
BUG_FLAGS = ["-ccompat", "-Wall", "-Wextra", "-Wuninitialized", "-Wmaybe-uninitialized", "-Wdangling-pointer", "-Wnull-dereference",
             "-Wformat-security"]
STACK_FLAGS = ["-ccompat", "-Wframe-larger-than=1024", "-Wstack-usage=1024", "-Wvla", "-fstack-usage",
               "-fcallgraph-info=su"]


def _libc_flags():
    gcc = shutil.which("arm-none-eabi-gcc")
    if not gcc:
        pytest.skip("needs arm-none-eabi-gcc for the newlib headers")
    sysroot = subprocess.run([gcc, "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft", "--print-sysroot"],
                             capture_output=True, text=True).stdout.strip()
    inc = CURRENT_DIR / "libc_includes"
    # system headers (cross_includes.py): newlib's static helpers are no
    # -Wunused-function of the examples'
    return ["-nostdinc", f"-isystem{TOP / 'include'}", f"-isystem{inc}", f"-isystem{CURRENT_DIR / 'libc_imports'}",
            f"-isystem{inc / 'newlib'}", f"-isystem{sysroot}/include"]


def _compile(tmp_path, name, flags, obj="x.o"):
    """compile examples/checks/NAME in tmp_path (the .su / .ci land there);
    stderr with the directory taken off the file names"""
    proc = subprocess.run([str(TCC), *_libc_flags(), *flags, "-c", str(EXAMPLES / name), "-o", str(tmp_path / obj)],
                          capture_output=True, text=True, cwd=tmp_path)
    return proc.returncode, proc.stderr.replace(f"{EXAMPLES}/", "")


def _want(name):
    return (EXAMPLES / name).read_text()


def _mask(text):
    """the numbers (and -O0's SP moves inside a body) taken out"""
    text = re.sub(r"\d+", "N", _libtcc1_names(text)).replace("[dynamic,bounded]", "[static]")
    return re.sub(r" +", " ", text)


def _libtcc1_names(text):
    """a YasOS cross calls libtcc1's own memset (__tcc_memset)"""
    return re.sub(r"__tcc_(mem\w+)", r"\1", text)


def _yasos_cross():
    """the frames the .expected files give are a YasOS cross's (R9 kept): an
    ARM Linux one (the CI's) is compared with the numbers masked"""
    out = subprocess.run([str(TCC), "-dM", "-E", "-"], input="", capture_output=True, text=True).stdout
    return "__YasOS__" in out


def _exact(opt):
    return opt == "-O2" and _yasos_cross()


@pytest.mark.parametrize("opt", LEVELS)
def test_find_the_bugs_says_its_diag(tmp_path, opt):
    rc, err = _compile(tmp_path, "01_find_the_bugs.c", [opt, *BUG_FLAGS])
    assert rc == 0, err
    assert err == _want("01_find_the_bugs.diag")


def test_every_function_has_its_bug():
    """one warning per function, at a line of that function"""
    src = _want("01_find_the_bugs.c").splitlines()
    starts = [i + 1 for i, l in enumerate(src) if re.match(r"^\w[\w *]*\(.*\)$", l)]
    warned = {int(l.split(":")[1]) for l in _want("01_find_the_bugs.diag").splitlines() if ": warning: " in l}
    for a, b in zip(starts, starts[1:] + [len(src) + 1]):
        assert any(a <= w < b for w in warned), f"no warning in the function at line {a}"


@pytest.mark.parametrize("opt", LEVELS)
def test_fixed_is_clean(tmp_path, opt):
    rc, err = _compile(tmp_path, "01_fixed.c", [opt, *BUG_FLAGS, "-Werror"])
    assert rc == 0 and err == "", err


def _run(tmp_path, name, flags, extra_objs=None, tag=""):
    cfg = CompileConfig(extra_cflags=flags, output_dir=tmp_path / "build", output_suffix=tag, extra_objs=extra_objs)
    res = compile_testcase([EXAMPLES / name], MACHINE, config=cfg)
    assert res.success, res.error
    return subprocess.run(build_qemu_command(MACHINE, res.elf_file), shell=True, capture_output=True, text=True,
                          timeout=60)


@pytest.mark.parametrize("opt", LEVELS)
def test_fixed_runs(tmp_path, opt):
    proc = _run(tmp_path, "01_fixed.c", f"{opt} -Wall -Werror")
    assert proc.returncode == 0 and proc.stdout == _want("01_fixed.expect"), proc.stdout + proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_stack_budget_warnings(tmp_path, opt):
    rc, err = _compile(tmp_path, "02_stack_budget.c", [opt, *STACK_FLAGS], obj="02_stack_budget.o")
    assert rc == 0, err
    want = _want("02_stack_budget.diag")
    # -O2's numbers; -O0's frames are bigger, the warnings the same
    assert (err if _exact(opt) else _mask(err)) == (want if _exact(opt) else _mask(want))
    su = (tmp_path / "02_stack_budget.su").read_text()
    assert re.search(r"(?m):sum_squares\t\d+\tdynamic$", su), su
    assert re.search(r"(?m):checksum_big\t\d{4}\tstatic$", su), su


@pytest.mark.parametrize("opt", LEVELS)
def test_stack_budget_script(tmp_path, opt):
    rc, err = _compile(tmp_path, "02_stack_budget.c", [opt, *STACK_FLAGS], obj="02_stack_budget.o")
    assert rc == 0, err
    cmd = [sys.executable, str(TOP / "scripts" / "stack_usage.py"), "--libtcc1", "--unknown-budget", "256",
           "-e", "run_unbounded", "-e", "run_bounded", "02_stack_budget.ci"]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=tmp_path)
    assert proc.returncode == 0, proc.stderr
    want = _want("02_stack_budget.stack")
    assert (proc.stdout if _exact(opt) else _mask(proc.stdout)) == (want if _exact(opt) else _mask(want))
    # --limit: the bounded entry fits 1 KB, the other does not
    limit = cmd[:-5] + ["--limit", "1024", "-e"]
    assert subprocess.run(limit + ["run_bounded", "02_stack_budget.ci"], cwd=tmp_path,
                          capture_output=True).returncode == 0
    assert subprocess.run(limit + ["run_unbounded", "02_stack_budget.ci"], cwd=tmp_path,
                          capture_output=True).returncode == 1


@pytest.mark.parametrize("opt", LEVELS)
def test_stack_budget_runs(tmp_path, opt):
    proc = _run(tmp_path, "02_stack_budget.c", f"{opt} -Wall -Werror")
    assert proc.returncode == 0 and proc.stdout == _want("02_stack_budget.expect"), proc.stdout + proc.stderr


def _ubsan_runtime(tmp_path):
    out = tmp_path / "ubsan_runtime.o"
    proc = subprocess.run([str(TCC), "-ccompat", "-Wall", "-Werror", "-nostdlib", "-fno-pic", "-mno-sb-relative-got",
                           "-O1", "-c", *INCLUDE_FLAGS, str(CURRENT_DIR / "ubsan_runtime.c"), "-o", str(out)],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return out


SAN_EXPECT = _want("03_sanitizers.expect").splitlines() if (EXAMPLES / "03_sanitizers.expect").exists() else []
SAN_OUT = [l for l in SAN_EXPECT if "runtime error" not in l]
SAN_ERR = [l for l in SAN_EXPECT if "runtime error" in l]


@pytest.mark.parametrize("opt", LEVELS)
def test_sanitizers_off(tmp_path, opt):
    proc = _run(tmp_path, "03_sanitizers.c", f"{opt} -Wall -Werror")
    assert proc.stdout.splitlines() == SAN_OUT and "runtime error" not in proc.stderr, proc.stdout + proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_sanitizers_handler_mode(tmp_path, opt):
    proc = _run(tmp_path, "03_sanitizers.c", f"{opt} -Wall -Werror -fsanitize=undefined -fsanitize-recover=undefined",
                extra_objs=[_ubsan_runtime(tmp_path)])
    assert proc.stdout.splitlines() == SAN_OUT, proc.stdout + proc.stderr
    reports = [l.rsplit("/", 1)[-1] for l in proc.stderr.splitlines() if "runtime error" in l]
    assert reports == SAN_ERR, proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_sanitizers_trap_mode(tmp_path, opt):
    proc = _run(tmp_path, "03_sanitizers.c", f"{opt} -Wall -Werror -fsanitize=undefined")
    out = proc.stdout + proc.stderr
    assert proc.stdout.splitlines() == SAN_OUT[:1], out
    # the check's UDF: UNDEFINSTR, not some other fault
    assert "HardFault" in out and "CFSR=0x00010000" in out, out
