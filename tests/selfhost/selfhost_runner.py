"""Helpers for the self-host bootstrap gate.

This module is intentionally free of pytest imports so it can be reused from
scripts or ad-hoc debugging.
"""

import re
import subprocess
import sys
from pathlib import Path

# Make tests/ir_tests/qemu_run.py importable for cross-reference runs.
IR_TESTS_DIR = Path(__file__).parent.parent / "ir_tests"
if str(IR_TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(IR_TESTS_DIR))

from qemu_run import compile_testcase, prepare_test, CompileConfig  # noqa: E402


MACHINE = "mps2-an505"


class GuestUnavailable(RuntimeError):
    """The YasOS guest never ran the command, so nothing was measured.

    Distinct from a run that produced the wrong answer: a caller should skip on
    this, not fail.  The usual cause is the kernel image in zig-out/bin being
    built for a different QEMU machine than the FAT harness boots (it hardcodes
    mps2-an505 and the fatdisk window of that board), which locks the guest up
    in early boot and takes the serial pty down with it.
    """


# Seen in the FAT runner's output when the guest died rather than answered.
# qemu_fatdisk_run.py echoes the QEMU log when the process exits before the pty
# appears; once it is gone, pyserial reports EIO on the dead pty.
_GUEST_DEAD_MARKERS = (
    "qemu: fatal",
    "Lockup:",
    "Input/output error",
    "no pty",
)


def _find_python_with_serial():
    """Return a python interpreter that has pyserial installed."""
    for exe in [sys.executable, "/usr/bin/python3", "python3"]:
        try:
            result = subprocess.run(
                [exe, "-c", "import serial"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if result.returncode == 0:
                return exe
        except FileNotFoundError:
            continue
    raise RuntimeError(
        "qemu_fatdisk_run.py requires pyserial, but no python interpreter "
        "with the 'serial' module was found."
    )


def _base_cross_cflags():
    """Default cross-compile flags matching the QEMU ir_tests harness."""
    return [
        "-O1",
        "-nostdlib",
        "-fvisibility=hidden",
        "-mcpu=cortex-m33",
        "-mthumb",
        "-mfloat-abi=soft",
        "-ffunction-sections",
    ]


def compile_tinycc_source(compiler, sources, include_dirs, output_dir, extra_defines=()):
    """Compile each tinycc source file to a relocatable object with the cross compiler.

    Returns a dict mapping source path -> object path.  Raises RuntimeError on
    the first failure so the smoke gate fails loudly.
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    results = {}

    base_cmd = [str(compiler)] + _base_cross_cflags() + ["-c", "-Werror"]
    for inc in include_dirs:
        base_cmd.extend(["-I", str(inc)])
    for d in extra_defines:
        base_cmd.append(f"-D{d}")

    for src in sources:
        src = Path(src)
        obj = output_dir / f"{src.stem}.o"
        cmd = base_cmd + [str(src), "-o", str(obj)]
        result = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"Self-host compile failed for {src.name}:\n"
                f"{' '.join(cmd)}\n{result.stdout}"
            )
        results[src] = obj
    return results


def _load_expect(test_file):
    """Return (expected_lines, expected_exit_code) from a tests2 .expect file."""
    expect_file = Path(test_file).with_suffix(".expect")
    if not expect_file.exists():
        return [], 0

    lines = []
    exit_code = 0
    returns_re = re.compile(r"^\[returns (\d+)\]$")
    with open(expect_file, "r", encoding="utf-8") as f:
        for line in f:
            stripped = line.rstrip("\n")
            m = returns_re.match(stripped)
            if m:
                exit_code = int(m.group(1))
            else:
                lines.append(stripped)
    return lines, exit_code


def run_cross_reference(test_file, output_dir, timeout=10):
    """Compile and run a tests2 case with the cross compiler via QEMU.

    Returns (stdout_lines, exit_code).  This is the reference output against
    which the native compiler run is compared.
    """
    test_file = Path(test_file).resolve()
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    expected_lines, expected_exit = _load_expect(test_file)
    config = CompileConfig(
        extra_cflags="-O1",
        output_suffix="_cross_ref",
        output_dir=output_dir,
        timeout=120,
    )
    compile_result = compile_testcase([test_file], MACHINE, config=config)
    if not compile_result.success:
        raise RuntimeError(
            f"Cross reference build failed for {test_file.name}:\n{compile_result.error}"
        )

    sut = prepare_test(MACHINE, compile_result.elf_file)
    stdout_lines = []
    try:
        # The SubprocessSUT wrapper used by prepare_test buffers output; read
        # until the process exits and then collect everything.
        sut.wait(timeout=timeout)
        if sut._proc.stdout is not None:
            data = sut._proc.stdout.read()
            if data:
                text = data.decode("utf-8", errors="replace")
                stdout_lines = text.replace("\r\n", "\n").replace("\r", "\n").splitlines()
    finally:
        sut.close()

    # Reconcile exit code: .expect [returns N] overrides the default.
    return stdout_lines, expected_exit


def run_native_via_fat(
    yasos_root,
    native_tcc,
    test_file,
    output_dir,
    timeout=60,
):
    """Run a tests2 case compiled by the native tcc inside YasOS via FAT drive.

    The native compiler is invoked on the guest as ``/usr/bin/tcc`` if it has
    been installed into the rootfs; otherwise the provided ``native_tcc`` path
    is copied onto the FAT image and run from ``/mnt/TCC``.

    Returns (stdout_lines, exit_code).
    """
    test_file = Path(test_file)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    runner = yasos_root / "scripts" / "qemu_fatdisk_run.py"
    if not runner.is_file():
        raise RuntimeError(f"FAT-drive runner not found: {runner}")

    # qemu_fatdisk_run.py needs pyserial, which is often only installed for the
    # system python.  Pick a python interpreter that can import serial.
    fat_python = _find_python_with_serial()

    # Decide whether to use the installed /usr/bin/tcc or a FAT-mounted binary.
    installed_tcc = yasos_root / "rootfs" / "usr" / "bin" / "tcc"
    if installed_tcc.is_file():
        tcc_cmd = "/usr/bin/tcc"
    else:
        tcc_cmd = "/mnt/TCC"

    # The native compiler targets YasOS, so its default output format is YAFF.
    # Use lowercase 8.3 names so tcc recognizes the .c extension.
    guest_cmd = (
        f"{tcc_cmd} /mnt/in.c -o /mnt/out "
        "-I/usr/include -L/usr/lib -L/lib "
        "&& /mnt/out; echo RC=$?"
    )

    cmd = [
        fat_python,
        str(runner),
        "--put",
        f"{test_file}:in.c",
        "--cmd",
        guest_cmd,
        "--timeout",
        str(timeout),
        "--boot-wait",
        "7",
    ]
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        timeout=timeout + 30,
    )

    stdout = result.stdout
    stdout_lines = stdout.replace("\r\n", "\n").replace("\r", "\n").splitlines()

    # qemu_fatdisk_run.py streams guest output to stdout.  Extract RC=N line.
    rc_re = re.compile(r"RC=(\d+)")
    exit_code = None
    for line in reversed(stdout_lines):
        m = rc_re.search(line)
        if m:
            exit_code = int(m.group(1))
            break

    if exit_code is None:
        if any(marker in stdout for marker in _GUEST_DEAD_MARKERS):
            raise GuestUnavailable(
                f"YasOS guest did not boot under QEMU {MACHINE}\n"
                f"FAT runner output:\n{stdout}"
            )
        raise RuntimeError(
            f"Could not determine native exit code for {test_file.name}.\n"
            f"FAT runner output:\n{stdout}"
        )

    return stdout_lines, exit_code


def _extract_program_output(fat_stdout_lines):
    """Extract the program's stdout from qemu_fatdisk_run.py output.

    The guest prints the shell command, then the program output, then
    ``RC=N``.  Everything outside that window is shell/QEMU/FAT noise.
    """
    # Find the last line that looks like the shell command we sent.
    cmd_idx = None
    for i, line in enumerate(fat_stdout_lines):
        if "/mnt/in.c -o /mnt/out" in line and "echo RC=$?" in line:
            cmd_idx = i
    if cmd_idx is None:
        return []

    # Collect lines after the command until RC=N.
    program_lines = []
    for line in fat_stdout_lines[cmd_idx + 1 :]:
        s = line.rstrip()
        if s.startswith("RC="):
            break
        if s.startswith("$"):
            break
        program_lines.append(s)
    return program_lines


def normalize_output(lines, *, from_fat_runner=False):
    """Drop non-deterministic / non-comparable lines from captured output."""
    if from_fat_runner:
        lines = _extract_program_output(lines)

    filtered = []
    for line in lines:
        s = line.rstrip()
        # Drop the shell prompt, RC marker, and QEMU/serial noise.
        if s.startswith("$") or s.startswith("#"):
            continue
        if s.startswith("RC="):
            continue
        if s.startswith(">> "):
            continue
        if s.startswith("qemu pty="):
            continue
        if not s:
            continue
        filtered.append(s)
    return filtered
