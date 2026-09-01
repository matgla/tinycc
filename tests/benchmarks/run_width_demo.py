#!/usr/bin/env python3
"""
Build and run the instruction-width demo on real RP2350 hardware.

The image pairs hand-written Thumb-2 sequences that compute identical results
by different instruction selections, and reports bytes, instruction count and
DWT cycles for each.  It answers, with measurements rather than folklore,
whether code size ranks sequences by speed on this core.

Hardware only, and for the same reason as run_fp_conformance.py: QEMU has no
RP2350 machine and models no Cortex-M33 timing at all, so an emulated cycle
count here would be a number about QEMU.

Examples:
    ./run_width_demo.py 192.168.0.113
    ./run_width_demo.py 192.168.0.113 --identity ~/.ssh/id_rsa
    ./run_width_demo.py --serial-log /tmp/width.log        # board on this node

Board wiring, OpenOCD setup and SSH expectations are identical to
run_benchmark.py -- see RP2350_README.md.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Reuse run_benchmark.py's flash/serial machinery rather than duplicating the
# OpenOCD + serial-port-detection logic, which is fiddly and already debugged.
sys.path.insert(0, str(Path(__file__).parent))
from run_benchmark import upload_and_run, is_local_host, _print_hardfault_details  # noqa: E402

HERE = Path(__file__).parent.resolve()
SDK_PATH = HERE / "libs" / "pico-sdk"
BUILD_DIR = HERE / "build_width_demo"


def build_image(clean: bool) -> Path:
    """Configure and build the width_demo image; return the .elf path."""
    if clean and BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)

    env = dict(os.environ)
    # pico_sdk_import.cmake reads the *environment* variable, not the cache
    # entry, so passing -DPICO_SDK_PATH alone silently fails to find the SDK.
    env["PICO_SDK_PATH"] = str(SDK_PATH)

    cfg = [
        "cmake", "-S", str(HERE), "-B", str(BUILD_DIR),
        f"-DPICO_SDK_PATH={SDK_PATH}",
        "-DPICO_PLATFORM=rp2350",
        "-DCMAKE_BUILD_TYPE=Release",
        # The demo is hand-written assembly, so the benchmark library's
        # compiler is irrelevant to it -- but the shared CMakeLists still runs
        # its selection logic, and GCC is the branch with no armv8m-tcc
        # prerequisite.
        "-DBENCHMARK_COMPILER=GCC",
        "-DENABLE_MIBENCH=OFF",
        "-DBUILD_WIDTH_DEMO=ON",
    ]
    print("[build] configuring width_demo ...")
    r = subprocess.run(cfg, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        raise SystemExit("cmake configure failed")

    print("[build] compiling width_demo ...")
    r = subprocess.run(["cmake", "--build", str(BUILD_DIR), "--target", "width_demo", "-j", "8"],
                       env=env, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        raise SystemExit("build failed")

    elf = BUILD_DIR / "width_demo.elf"
    if not elf.exists():
        raise SystemExit(f"expected image not produced: {elf}")
    return elf


def dump_encodings(elf: Path) -> None:
    """Disassemble the kernels so the byte counts the board prints can be
    checked against the encodings the assembler actually chose.  The board's
    numbers are label arithmetic over the same encodings, so this is a second
    view of one fact, not a second source -- but it is the view that shows
    *which* instructions went narrow."""
    objdump = shutil.which("arm-none-eabi-objdump")
    if not objdump:
        print("[dis] arm-none-eabi-objdump not found, skipping")
        return
    r = subprocess.run([objdump, "-d", "--no-show-raw-insn", str(elf)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return
    keep = False
    for line in r.stdout.splitlines():
        m = re.match(r"^[0-9a-f]+ <(k_\w+)>:", line)
        if m:
            keep = True
            print(f"\n  {m.group(1)}:")
            continue
        if keep:
            if not line.strip():
                keep = False
                continue
            print(f"    {line.strip()}")


VERDICT = re.compile(r"WIDTH DEMO: (PASS|FAIL) mismatches=(\d+)")


def report(output: str) -> int:
    """Print the board's own table verbatim; return a process exit code.

    No re-derivation here on purpose: the board computed bytes, cycles and the
    comparisons, and reformatting them on the host would only add a place for
    the two to disagree.
    """
    started = False
    for line in output.splitlines():
        if "width demo:" in line:
            started = True
        if started:
            print(line.rstrip())
        if "benchmark stopped" in line:
            break

    v = VERDICT.search(output)
    if not v:
        print("\n  no verdict found in serial output")
        _print_hardfault_details(output)
        return 1
    return 0 if v.group(1) == "PASS" else 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host", nargs="?", default=None,
                   help="SSH host with the board attached; omit to run locally")
    p.add_argument("--port", type=int, default=22, help="SSH port (default: 22)")
    p.add_argument("--identity", "-i", help="SSH identity file")
    p.add_argument("--password", help="SSH password")
    p.add_argument("--clean", action="store_true", help="wipe the build dir first")
    p.add_argument("--skip-build", action="store_true", help="reuse the existing image")
    p.add_argument("--disasm", action="store_true",
                   help="also print the kernels' disassembly")
    p.add_argument("--serial-log", help="write the raw serial capture here")
    args = p.parse_args()

    if args.skip_build:
        elf = BUILD_DIR / "width_demo.elf"
        if not elf.exists():
            raise SystemExit(f"--skip-build given but {elf} does not exist")
    else:
        elf = build_image(args.clean)

    if args.disasm:
        dump_encodings(elf)

    where = "locally" if (args.host is None or is_local_host(args.host)) else f"on {args.host}"
    print(f"\n[run] flashing and running {where} ...")
    ok, output, errors = upload_and_run(elf, args.host, args.port,
                                        identity=args.identity, password=args.password)

    if args.serial_log:
        Path(args.serial_log).write_text(output)

    if not ok:
        print("[run] board run failed")
        if errors:
            print(errors)
        _print_hardfault_details(output)
        return 1

    print()
    return report(output)


if __name__ == "__main__":
    sys.exit(main())
