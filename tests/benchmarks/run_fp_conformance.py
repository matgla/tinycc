#!/usr/bin/env python3
"""
Run the IEEE-754 floating-point conformance suite on real RP2350 hardware.

Why this exists separately from run_benchmark.py: QEMU has no RP2350 machine
at all, so it cannot model the double coprocessor.  Hardware is the *only*
place DCP codegen can be validated, which makes this the acceptance test for
the whole DCP effort.  The soft-float path is also covered under QEMU by
tests/ir_tests/421_fp_conformance.c; running the identical vectors here proves
the two agree.

The expected results are host-generated bit patterns (see
tests/fp/gen_fp_vectors.c), not a TCC-vs-GCC diff, so a bug present in both
compilers still fails.

Examples:
    # soft float, TCC -O1  (the baseline)
    ./run_fp_conformance.py 192.168.0.113 --identity ~/.ssh/id_rsa

    # RP2350 DCP runtime library, still with __aeabi_* calls
    ./run_fp_conformance.py 192.168.0.113 --fp-lib rp2350fp

    # inline DCP/VFP codegen instead of library calls
    ./run_fp_conformance.py 192.168.0.113 --fp-lib rp2350fp --mfpu rp2350

    # GCC reference build, for comparison
    ./run_fp_conformance.py 192.168.0.113 --compiler GCC

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
TCC_ROOT = (HERE / "../..").resolve()
SDK_PATH = HERE / "libs" / "pico-sdk"


def build_image(compiler: str, opt_level: str, fp_lib: str, mfpu: str, clean: bool) -> Path:
    """Configure and build the fp_conformance image; return the .elf path."""
    suffix = "tcc" if compiler == "TCC" else "gcc"
    tag = f"{suffix}_O{opt_level}_{fp_lib}" + (f"_{mfpu}" if mfpu else "")
    build_dir = HERE / f"build_fp_{tag}"

    if clean and build_dir.exists():
        shutil.rmtree(build_dir)

    env = dict(os.environ)
    # pico_sdk_import.cmake reads the *environment* variable, not the cache
    # entry, so passing -DPICO_SDK_PATH alone silently fails to find the SDK.
    env["PICO_SDK_PATH"] = str(SDK_PATH)

    cfg = [
        "cmake", "-S", str(HERE), "-B", str(build_dir),
        f"-DPICO_SDK_PATH={SDK_PATH}",
        "-DPICO_PLATFORM=rp2350",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DBENCHMARK_COMPILER={compiler}",
        f"-DBENCHMARK_OPT_LEVEL={opt_level}",
        "-DBUILD_FP_CONFORMANCE=ON",
        f"-DFP_LIB={fp_lib}",
        f"-DFP_MFPU={mfpu}",
    ]
    print(f"[build] configuring {tag} ...")
    r = subprocess.run(cfg, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        raise SystemExit(f"cmake configure failed for {tag}")

    print(f"[build] compiling {tag} ...")
    target = f"fp_conformance_{suffix}"
    r = subprocess.run(["cmake", "--build", str(build_dir), "--target", target, "-j", "8"],
                       env=env, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        raise SystemExit(f"build failed for {tag}")

    elf = build_dir / f"{target}.elf"
    if not elf.exists():
        raise SystemExit(f"expected image not produced: {elf}")

    size = subprocess.run(["arm-none-eabi-size", str(elf)], capture_output=True, text=True)
    print(size.stdout.strip())
    return elf


FAIL_LINE = re.compile(r"^FP FAIL .*$", re.MULTILINE)
BYOP_LINE = re.compile(r"^FP BY-OP .*$", re.MULTILINE)
SUBNORMAL = re.compile(r"FP conformance: (\d+) of (\d+) failures involve subnormals")
SUMMARY = re.compile(r"FP conformance: double (\d+) failed / (\d+), float (\d+) failed / (\d+)")
VERDICT = re.compile(r"FP CONFORMANCE: (PASS|FAIL)(?: failures=(\d+))?")
BUILD_ID = re.compile(r"FP conformance build: (\S+)")


def report(output: str, allow_ftz: bool = False) -> int:
    """Print a digest of the board's serial output; return the failure count.

    Returns -1 when the run produced no usable verdict at all, which is a
    different problem from a failing vector (board never booted, HardFault,
    serial never synced) and must not be reported as a pass.
    """
    bid = BUILD_ID.search(output)
    if bid:
        print(f"  build:  {bid.group(1)}")

    m = SUMMARY.search(output)
    if m:
        df, dt, ff, ft = (int(x) for x in m.groups())
        print(f"  double: {dt - df}/{dt} passed")
        print(f"  float:  {ft - ff}/{ft} passed")

    for line in FAIL_LINE.findall(output)[:20]:
        print(f"  {line}")

    # The board caps raw FP FAIL lines (FP_MAX_REPORTED_FAILURES), so the
    # per-op histogram is the only complete picture of what failed.
    hist = [ln for ln in BYOP_LINE.findall(output) if not ln.endswith("ok")]
    if hist:
        print()
        for line in hist:
            print(f"  {line}")

    v = VERDICT.search(output)
    if not v:
        print("  no verdict found in serial output")
        _print_hardfault_details(output)
        return -1

    if v.group(1) == "PASS":
        print("  VERDICT: PASS")
        return 0

    n = int(v.group(2) or 0)

    # The RP2350 DCP flushes subnormals to zero and has no path that doesn't --
    # pico-sdk's own DCP routines don't either. --allow-ftz accepts exactly that
    # deviation and nothing more, so a real regression still turns the gate red.
    if allow_ftz:
        s = SUBNORMAL.search(output)
        if not s:
            print("  VERDICT: FAIL -- --allow-ftz given but the image reports no "
                  "subnormal breakdown (rebuild tests/fp/fp_conformance.c)")
            return n
        sub, tot = int(s.group(1)), int(s.group(2))
        rest = tot - sub
        if rest == 0:
            print(f"  VERDICT: PASS with FTZ ({sub} subnormal-flush deviations, 0 other)")
            return 0
        print(f"  VERDICT: FAIL ({rest} non-subnormal vectors; {sub} subnormal ignored)")
        return rest

    print(f"  VERDICT: FAIL ({n} vectors)")
    return n


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host", nargs="?", default=None,
                   help="SSH host with the board attached; omit to run locally")
    p.add_argument("--port", type=int, default=22, help="SSH port (default: 22)")
    p.add_argument("--identity", "-i", help="SSH identity file")
    p.add_argument("--password", help="SSH password")
    p.add_argument("--compiler", choices=["TCC", "GCC"], default="TCC")
    p.add_argument("--opt-level", "-O", default="1", choices=["0", "1", "2"])
    p.add_argument("--fp-lib", default="softfp",
                   choices=["softfp", "rp2350fp", "vfpv4sp", "vfpv5dp"],
                   help="FP runtime library from lib/fp (default: softfp)")
    p.add_argument("--allow-ftz", action="store_true",
                   help="treat subnormal-flush deviations as expected (the RP2350 DCP "
                        "has no subnormal support); still fails on anything else")
    p.add_argument("--mfpu", default="",
                   help="value for TCC's -mfpu= (e.g. rp2350 for inline DCP/VFP)")
    p.add_argument("--clean", action="store_true", help="wipe the build dir first")
    p.add_argument("--skip-build", action="store_true", help="reuse the existing image")
    p.add_argument("--serial-log", help="write the raw serial capture here")
    args = p.parse_args()

    elf = build_image(args.compiler, args.opt_level, args.fp_lib, args.mfpu,
                      args.clean and not args.skip_build)

    where = "locally" if (args.host is None or is_local_host(args.host)) else f"on {args.host}"
    print(f"[run] flashing and running {where} ...")
    ok, output, errors = upload_and_run(elf, args.host, args.port,
                                        identity=args.identity, password=args.password)

    if args.serial_log:
        Path(args.serial_log).write_text(output)
        print(f"[run] serial log written to {args.serial_log}")

    if not ok:
        print("[run] flash/run failed")
        if errors:
            print(errors)
        return 2

    print("\nFP conformance results:")
    failures = report(output, allow_ftz=args.allow_ftz)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
