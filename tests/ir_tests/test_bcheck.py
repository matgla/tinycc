"""-b: bounds checking on the armv8m cross, run under QEMU.

The frontend emits ordinary calls through the IR (frontend/gen/op/bcheck.c):
__bound_ptr_add for pointer arithmetic, turned into __bound_ptr_indirN when
its result is dereferenced, __bound_new_region / __bound_delete_region for
local arrays and structs, and a .bounds table of the globals that the linker
ends and names __bound_globals.  The runtime is lib/bcheck-bm.c (newlib,
bare metal), compiled here without -b and linked in by hand: the harness
links with -nostdlib.  The device compiler has none of it
(CONFIG_TCC_BCHECK_CROSS, the cross only).

Each case of bcheck/bcheck_cases.c prints "go <case>", then "ok <case> <value>"
unless the runtime stopped it with a BCHECK line and exit code 255.
"""

import re
import subprocess
from pathlib import Path

import pytest

from qemu_run import CompileConfig, build_qemu_command, compile_testcase

CURRENT_DIR = Path(__file__).parent
ROOT = CURRENT_DIR.parent.parent
TCC = ROOT / "armv8m-tcc"
MACHINE = "mps2-an505"
LEVELS = ["-O0", "-O1", "-O2", "-Os"]
CASES = CURRENT_DIR / "bcheck" / "bcheck_cases.c"
RUNTIME = ROOT / "lib" / "bcheck-bm.c"

# the value each correct case returns, the same with and without -b
OK = {
    "local_ok": 4,
    "global_ok": 7,
    "struct_ok": 0,
    "heap_ok": 0,
    "walk_ok": 17,
    "realloc_ok": 6,
    "calloc_ok": 0,
    "inline_ok": 13,
    "vla_ok": 15,
    "str_ok": 5,
    "copy_ok": 7,
    "frames_ok": 17,
    "static_ok": 9,
}
OUTSIDE = r"BCHECK: 0x[0-9a-f]+ is outside of the region \(0x[0-9a-f]+\.\.0x[0-9a-f]+\)"
CAUGHT = {
    "local_write": OUTSIDE + ", at pc",
    "local_read": OUTSIDE + ", at pc",
    "local_under": OUTSIDE + ", at pc",
    "inline_read": OUTSIDE + ", at pc",
    "global_write": OUTSIDE + ", at pc",
    "global_read": OUTSIDE + ", at pc",
    "struct_write": OUTSIDE + ", at pc",
    "heap_write": OUTSIDE + ", at pc",
    "calloc_write": OUTSIDE + ", at pc",
    "walk_end": OUTSIDE + ", at pc",
    "use_after_free": OUTSIDE + ": use after free, at pc",
    "realloc_old": OUTSIDE + ": use after free, at pc",
    "double_free": r"BCHECK: freeing invalid region, at pc",
}

BASE_FLAGS = ["-ccompat", "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft", "-fno-pic", "-mno-sb-relative-got"]


def _has_b():
    if not TCC.exists():
        return False
    proc = subprocess.run([str(TCC), "-b", "-E", "-x", "c", "-"], input="", capture_output=True, text=True)
    return proc.returncode == 0


pytestmark = pytest.mark.skipif(not _has_b(), reason="needs armv8m-tcc with -b (configure without --disable-bcheck-cross)")


def _includes():
    sysroot = subprocess.run(["arm-none-eabi-gcc", "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft",
                              "--print-sysroot"], capture_output=True, text=True).stdout.strip()
    inc = CURRENT_DIR / "libc_includes"
    return [f"-I{inc}", f"-I{CURRENT_DIR / 'libc_imports'}", f"-I{inc / 'newlib'}", f"-I{sysroot}/include",
            f"-I{ROOT / 'include'}"]


@pytest.fixture(scope="module")
def runtime(tmp_path_factory):
    """lib/bcheck-bm.c compiled (without -b) at each level."""
    out = tmp_path_factory.mktemp("bcheck_rt")
    objs = {}
    for opt in LEVELS:
        obj = out / f"bcheck-bm{opt}.o"
        proc = subprocess.run([str(TCC), *BASE_FLAGS, "-nostdlib", "-ffunction-sections", opt, *_includes(), "-c",
                               str(RUNTIME), "-o", str(obj)], capture_output=True, text=True)
        assert proc.returncode == 0, proc.stderr
        objs[opt] = obj
    return objs


def _build(tmp_path, opt, runtime_obj, bcheck=True):
    cfg = CompileConfig(extra_cflags=opt + (" -b" if bcheck else ""), output_dir=tmp_path,
                        output_suffix=opt.replace("-", "_"), extra_objs=[str(runtime_obj)] if bcheck else None)
    res = compile_testcase([CASES], MACHINE, config=cfg)
    assert res.success, "\n".join(res.output_lines[-20:])
    return res.elf_file


def _run(elf, case):
    proc = subprocess.run(build_qemu_command(MACHINE, elf, [case]), shell=True, capture_output=True, text=True,
                          timeout=60)
    out = proc.stdout + proc.stderr
    assert f"go {case}" in out, out
    return proc.returncode, out


@pytest.mark.parametrize("opt", LEVELS)
def test_caught_and_clean(tmp_path, runtime, opt):
    elf = _build(tmp_path, opt, runtime[opt])
    for case, value in OK.items():
        rc, out = _run(elf, case)
        assert rc == 0 and re.search(rf"(?m)^ok {case} {value}$", out), (case, out)
        assert "BCHECK" not in out, (case, out)
    for case, pattern in CAUGHT.items():
        rc, out = _run(elf, case)
        assert rc == 255, (case, rc, out)
        assert re.search(pattern, out), (case, out)
        assert f"ok {case}" not in out, (case, out)


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_same_values_without_b(tmp_path, runtime, opt):
    """The correct cases compute what they compute without -b."""
    elf = _build(tmp_path, opt, runtime[opt], bcheck=False)
    for case, value in OK.items():
        rc, out = _run(elf, case)
        assert rc == 0 and re.search(rf"(?m)^ok {case} {value}$", out), (case, out)


def _symbols(tmp_path, src, *flags):
    obj = tmp_path / "t.o"
    proc = subprocess.run([str(TCC), *BASE_FLAGS, *flags, "-c", str(src), "-o", str(obj)], capture_output=True,
                          text=True)
    assert proc.returncode == 0, proc.stderr
    nm = subprocess.run(["arm-none-eabi-nm", str(obj)], capture_output=True, text=True).stdout
    sections = subprocess.run(["arm-none-eabi-readelf", "-SW", str(obj)], capture_output=True, text=True).stdout
    return nm, sections


@pytest.mark.parametrize("opt", LEVELS)
def test_calls_only_with_b(tmp_path, opt):
    src = tmp_path / "t.c"
    src.write_text(
        "int g[4];\n"
        "struct P { int x, y; };\n"
        "int f(int i, int *p, struct P *q) {\n"
        "  int a[4]; char c[3];\n"
        "  a[i] = p[i]; c[i] = 1; g[i] = q->y;\n"
        "  return a[0] + c[0] + g[1];\n"
        "}\n")
    nm, sections = _symbols(tmp_path, src, opt)
    assert "__bound" not in nm
    assert ".bounds" not in sections
    nm, sections = _symbols(tmp_path, src, opt, "-b")
    for sym in ("__bound_ptr_indir4", "__bound_ptr_indir1", "__bound_new_region", "__bound_delete_region"):
        assert sym in nm, (sym, nm)
    assert ".bounds" in sections


UNDER_B = [
    # a loop closed only through `goto *tab[i]`: with the bound check's call
    # in it, the exit value of the loop's variable folded to its start (no
    # CFG edge into the labels)
    "829_hazard_sandwich_cgoto.c", "bug_cfg_label_addr_edges.c",
    # an "m" operand's base (the checked pointer) in a register the asm
    # clobbers; an "=r" output homed in a clobbered callee-saved register
    "621_asm_memory_operand_frame.c", "623_asm_spilled_operand.c", "bug_asm_operand_in_clobbered_reg.c",
]


@pytest.mark.parametrize("src", UNDER_B)
@pytest.mark.parametrize("opt", LEVELS)
def test_programs_under_b(tmp_path, runtime, opt, src):
    """Programs whose -b build once went wrong print their .expect."""
    cfg = CompileConfig(extra_cflags=opt + " -b", output_dir=tmp_path, output_suffix=opt.replace("-", "_"),
                        extra_objs=[str(runtime[opt])])
    res = compile_testcase([CURRENT_DIR / src], MACHINE, config=cfg)
    assert res.success, "\n".join(res.output_lines[-20:])
    proc = subprocess.run(build_qemu_command(MACHINE, res.elf_file), shell=True, capture_output=True, text=True,
                          timeout=60)
    want = (CURRENT_DIR / src).with_suffix(".expect").read_text().split()
    assert proc.stdout.split()[:len(want)] == want, proc.stdout + proc.stderr
