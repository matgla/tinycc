"""YasOS libc's libgen.c, written with yasc slices, on QEMU (yasc_libgen_cases.c).

The host unit tests of libs/libc build with gcc, which cannot compile yasc, so
libgen is tested here: compiled by this tcc with libc's own headers (as the
rootfs builds it: tcc's safe default, no -ccompat) and linked to a
newlib program that checks every answer against glibc's.  The program is
built with -ffortify: libgen.h's null_terminated_string_arg checks each call,
so a buffer with no NUL ends in __chk_fail before libgen reads past it.
"""

import subprocess
from pathlib import Path

import pytest

from qemu_run import CompileConfig, build_qemu_command, compile_testcase
from test_fortify import _libc_dir

CURRENT_DIR = Path(__file__).parent
ROOT = CURRENT_DIR.parent.parent
TCC = ROOT / "armv8m-tcc"
MACHINE = "mps2-an505"
BASE_FLAGS = ["-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft", "-fno-pic", "-mno-sb-relative-got"]

pytestmark = [pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc"),
              pytest.mark.skipif(_libc_dir() is None, reason="no yasos libc (libs/libc or YASOS_LIBC_DIR)")]


def _libgen(tmp_path, opt):
    libc = _libc_dir()
    sysinc = libc.parent.parent / "source" / "sys" / "include"
    obj = tmp_path / f"libgen{opt}.o"
    proc = subprocess.run([str(TCC), "-Wall", "-Werror", "-nostdinc", f"-I{libc}", f"-I{sysinc}",
                           f"-I{ROOT / 'include'}", "-U_FORTIFY_SOURCE", *BASE_FLAGS, opt, "-c",
                           str(libc / "libgen.c"), "-o", str(obj)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    dis = subprocess.run(["arm-none-eabi-objdump", "-d", str(obj)], capture_output=True, text=True).stdout
    return obj, dis


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_libgen_on_qemu(tmp_path, opt):
    obj, dis = _libgen(tmp_path, opt)
    if opt != "-O0":
        assert "udf" not in dis, dis  # every slice check proven
    cfg = CompileConfig(extra_cflags=f"{opt} -ffortify", output_dir=tmp_path / "build", output_suffix=opt.strip("-"),
                        extra_objs=[str(obj)])
    res = compile_testcase([CURRENT_DIR / "yasc_libgen_cases.c"], MACHINE, config=cfg)
    assert res.success, res.error
    for arg, rc, want in (([], 0, "PASS 0"), (["chk"], 3, "chk_fail")):
        p = subprocess.run(build_qemu_command(MACHINE, res.elf_file, arg), shell=True, capture_output=True,
                           text=True, timeout=60)
        out = p.stdout + p.stderr
        assert want in out, out
