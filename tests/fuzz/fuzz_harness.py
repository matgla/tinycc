"""Shared QEMU plumbing for the differential-fuzzing scripts/tests (Tracks 2/3).

This module is the single place that knows how to:

  * detect whether the QEMU / newlib harness is usable in this environment;
  * compile a C program with ``armv8m-tcc`` at a given ``-O`` level and run it
    under QEMU ``mps2-an505``, capturing stdout + exit code  (reuses the
    ``tests/ir_tests/qemu_run.py`` plumbing -- ``compile_testcase`` /
    ``prepare_test`` / ``build_qemu_command`` -- exactly as ``test_qemu.py``
    drives a single test);
  * build the SAME program with ``arm-none-eabi-gcc`` into an equivalent
    semihosting ELF (same ``boot.S`` + ``linker_script.ld`` + newlib that the
    tcc path links against) and run it under the same QEMU, for the gcc oracle.

Both ``scripts/diff_olevels.py`` (Track 2) and ``scripts/diff_vs_gcc.py``
(Track 3), and their pytest wrappers, import from here so there is exactly one
runner.

Why a custom gcc link recipe?
-----------------------------
The ir_tests Makefile's plain-gcc branch links with ``--specs=rdimon.specs``,
which pulls gcc's own ``_start`` and conflicts with the board's ``boot.S``
(``_mainCRTStartup`` ends up undefined).  We instead reuse the board's
``boot.S`` + ``linker_script.ld`` and gcc's ``rdimon-crt0.o`` directly -- the
same components the tcc path uses -- so the gcc and tcc binaries boot
identically and only the *generated code* differs.  (The Makefile's
``-Wl,-oformat=elf32-littlearm`` must be omitted: under the gcc driver it makes
``ld`` silently emit nothing.)
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

# --- locate the repo + ir_tests plumbing ----------------------------------
THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent.parent                  # .../libs/tinycc
IR_TESTS_DIR = REPO_ROOT / "tests" / "ir_tests"
MACHINE_DIR = IR_TESTS_DIR / "qemu" / "mps2-an505"
NEWLIB_DIR = MACHINE_DIR / "newlib_build" / "arm-none-eabi" / "newlib"
LIBGLOSS_DIR = MACHINE_DIR / "newlib_build" / "arm-none-eabi" / "libgloss" / "arm"
TCC_BIN = REPO_ROOT / "armv8m-tcc"
MACHINE = "mps2-an505"

# Make the ir_tests helpers importable.
if str(IR_TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(IR_TESTS_DIR))

import qemu_run  # noqa: E402  (after sys.path tweak)
from qemu_run import (  # noqa: E402
    compile_testcase,
    CompileConfig,
    prepare_test,
    build_qemu_command,
)

GCC = "arm-none-eabi-gcc"
GCC_ABI_FLAGS = ["-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft"]

# Wall-clock seconds to wait for a guest program to reach exit() under QEMU.
# Generated programs are tiny and bounded; anything slower is a hang.
RUN_TIMEOUT = 10

# ASAN's LeakSanitizer makes the (instrumented) cross compiler exit non-zero on
# the *known* pre-existing frontend decl_initializer_alloc leak even when codegen
# is fine (see memory: yasos-tcc-ir-suite-asan-leak-blocks-validation).  That
# leak only affects the compiler process exit status, not the emitted object;
# the Makefile-driven tcc path is unaffected, and we set this for the direct gcc
# helper subprocesses for good measure.
_CHILD_ENV = dict(os.environ)
_CHILD_ENV.setdefault("ASAN_OPTIONS", "detect_leaks=0")


@dataclass
class RunResult:
    """Outcome of building + running one program variant under QEMU."""
    label: str            # e.g. "tcc-O2" or "gcc-O2"
    ok: bool              # build+run completed and produced output
    stdout: str           # normalised guest stdout (LF line endings)
    exit_code: Optional[int]
    error: str = ""       # populated when ok is False

    @property
    def signature(self) -> tuple:
        """The (stdout, exit_code) pair used for divergence comparison."""
        return (self.stdout.strip(), self.exit_code)


# ---------------------------------------------------------------------------
# Environment probing
# ---------------------------------------------------------------------------

def qemu_available() -> tuple[bool, str]:
    """Return (usable, reason).  ``usable`` is False with a clear reason when the
    QEMU / newlib harness is not prepared, so callers can skip cleanly."""
    if not TCC_BIN.exists():
        return False, f"armv8m-tcc not built ({TCC_BIN}); run 'make cross'"
    if shutil.which("qemu-system-arm") is None:
        return False, "qemu-system-arm not on PATH"
    if shutil.which(GCC) is None:
        return False, f"{GCC} not on PATH"
    if not (NEWLIB_DIR / "libc.a").exists():
        return False, (
            "newlib not prepared "
            f"({NEWLIB_DIR / 'libc.a'} missing); run 'make test-prepare' or "
            "tests/ir_tests/qemu/mps2-an505/build_newlib.sh"
        )
    if not (LIBGLOSS_DIR / "rdimon-crt0.o").exists():
        return False, f"libgloss rdimon-crt0.o missing ({LIBGLOSS_DIR})"
    return True, "ok"


def gcc_reference_available() -> tuple[bool, str]:
    """Track 3 also needs the gcc semihosting runtime pieces."""
    usable, reason = qemu_available()
    if not usable:
        return usable, reason
    for name in ("librdimon.a",):
        if not (LIBGLOSS_DIR / name).exists():
            return False, f"libgloss {name} missing ({LIBGLOSS_DIR})"
    return True, "ok"


# ---------------------------------------------------------------------------
# QEMU execution (shared by tcc and gcc paths)
# ---------------------------------------------------------------------------

def _run_elf(elf_file: Path, label: str) -> RunResult:
    """Run a prebuilt ELF under QEMU, capturing stdout + exit code.

    ``SubprocessSUT`` only fills its internal buffer while ``expect()`` reads the
    pipe, so we drive a drain loop that actively reads guest output until the
    process exits (or we hit the timeout), then read any final bytes.
    """
    if not Path(elf_file).exists():
        return RunResult(label, False, "", None, error=f"ELF missing: {elf_file}")
    sut = prepare_test(MACHINE, str(elf_file))
    try:
        deadline = time.monotonic() + RUN_TIMEOUT
        while time.monotonic() < deadline and not _sut_exited(sut):
            # A short expect() on an unlikely pattern actively pumps the pipe into
            # the buffer; TimeoutError just means "no match yet", which is fine.
            try:
                sut.expect(r"\x00THIS_PATTERN_NEVER_MATCHES\x00", timeout=0.2)
            except (TimeoutError, Exception):
                pass
        if not _sut_exited(sut):
            sut.close()
            return RunResult(label, False, _sut_buffer(sut), None,
                             error=f"hang: no exit within {RUN_TIMEOUT}s")
        # Drain any trailing bytes emitted just before exit.
        try:
            sut.expect(r"\x00THIS_PATTERN_NEVER_MATCHES\x00", timeout=0.1)
        except (TimeoutError, Exception):
            pass
        sut.close()
        return RunResult(label, True, _sut_buffer(sut), sut.exitstatus)
    finally:
        # prepare_test does not attach a logfile; nothing else to close.
        pass


def _sut_exited(sut) -> bool:
    if hasattr(sut, "_proc"):
        return sut._proc.poll() is not None
    if hasattr(sut, "isalive"):
        return not sut.isalive()
    return getattr(sut, "exitstatus", None) is not None


def _sut_buffer(sut) -> str:
    # SubprocessSUT accumulates guest output in ._buffer (already LF-normalised).
    buf = getattr(sut, "_buffer", "")
    return buf if isinstance(buf, str) else str(buf)


# ---------------------------------------------------------------------------
# tcc path -- reuse qemu_run.compile_testcase (Makefile-driven)
# ---------------------------------------------------------------------------

def run_with_tcc(source: Path, opt_level: str, out_dir: Path) -> RunResult:
    """Compile ``source`` with armv8m-tcc at ``opt_level`` and run under QEMU."""
    label = f"tcc{opt_level}"
    out_dir.mkdir(parents=True, exist_ok=True)
    suffix = "_" + opt_level.replace("-", "").replace(" ", "_")
    config = CompileConfig(
        extra_cflags=opt_level,
        output_dir=out_dir,
        output_suffix=suffix,
        clean_before_build=False,
    )
    result = compile_testcase([Path(source)], MACHINE, config=config)
    if not result.success:
        return RunResult(label, False, "", None,
                         error="tcc compile failed: " + (result.error or "").strip())
    return _run_elf(result.elf_file, label)


# ---------------------------------------------------------------------------
# gcc reference path -- custom semihosting link (boot.S + linker_script.ld)
# ---------------------------------------------------------------------------

def _gcc_path(flag: str) -> str:
    out = subprocess.run([GCC, *GCC_ABI_FLAGS, flag],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, env=_CHILD_ENV)
    return out.stdout.strip()


_GCC_RUNTIME_CACHE: dict = {}


def _gcc_runtime():
    if _GCC_RUNTIME_CACHE:
        return _GCC_RUNTIME_CACHE
    _GCC_RUNTIME_CACHE.update(
        libgcc=_gcc_path("-print-libgcc-file-name"),
        crti=_gcc_path("-print-file-name=crti.o"),
        crtend=_gcc_path("-print-file-name=crtend.o"),
        crtn=_gcc_path("-print-file-name=crtn.o"),
    )
    return _GCC_RUNTIME_CACHE


# The board boot object is identical for every program; build it once.
_BOOT_OBJ_CACHE: dict = {}


def _boot_obj(out_dir: Path) -> Path:
    key = str(out_dir)
    cached = _BOOT_OBJ_CACHE.get(key)
    if cached and Path(cached).exists():
        return Path(cached)
    boot_o = out_dir / "boot_gcc.o"
    rc = subprocess.run(
        [GCC, *GCC_ABI_FLAGS, "-ffunction-sections", "-c",
         str(MACHINE_DIR / "boot.S"), "-o", str(boot_o)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=_CHILD_ENV,
    )
    if rc.returncode != 0:
        raise RuntimeError("gcc failed to assemble boot.S:\n" + rc.stderr)
    _BOOT_OBJ_CACHE[key] = str(boot_o)
    return boot_o


def build_gcc_elf(source: Path, opt_level: str, out_dir: Path) -> tuple[Optional[Path], str]:
    """Build a semihosting ELF for ``source`` with arm-none-eabi-gcc.

    Returns (elf_path, error).  elf_path is None on failure.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    rt = _gcc_runtime()
    boot_o = _boot_obj(out_dir)
    stem = Path(source).stem + "_gcc" + opt_level.replace("-", "").replace(" ", "_")
    prog_o = out_dir / f"{stem}.o"
    elf = out_dir / f"{stem}.elf"

    # Compile the program object.
    cc = subprocess.run(
        [GCC, *GCC_ABI_FLAGS, "-ffunction-sections", *opt_level.split(),
         "-c", str(source), "-o", str(prog_o)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=_CHILD_ENV,
    )
    if cc.returncode != 0:
        return None, "gcc compile failed:\n" + cc.stderr

    libc = NEWLIB_DIR / "libc.a"
    libm = NEWLIB_DIR / "libm.a"
    librdimon = LIBGLOSS_DIR / "librdimon.a"
    rdimon_crt0 = LIBGLOSS_DIR / "rdimon-crt0.o"

    link = subprocess.run(
        [GCC, *GCC_ABI_FLAGS, "-nostdlib", "-ffunction-sections",
         str(prog_o), str(boot_o),
         rt["crti"], str(rdimon_crt0), rt["crtend"], rt["crtn"],
         "-o", str(elf),
         "-Wl,--gc-sections",
         "-Wl,--start-group", str(libc), str(librdimon), str(libm), rt["libgcc"],
         "-Wl,--end-group",
         "-T", str(MACHINE_DIR / "linker_script.ld")],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=_CHILD_ENV,
    )
    if link.returncode != 0 or not elf.exists():
        return None, "gcc link failed:\n" + link.stderr
    return elf, ""


def run_with_gcc(source: Path, opt_level: str, out_dir: Path) -> RunResult:
    """Build ``source`` with arm-none-eabi-gcc and run under QEMU."""
    label = f"gcc{opt_level}"
    elf, err = build_gcc_elf(source, opt_level, out_dir)
    if elf is None:
        return RunResult(label, False, "", None, error=err)
    return _run_elf(elf, label)
