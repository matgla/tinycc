from qemu_run import build_qemu_command, compile_testcase

import argparse
import subprocess
import sys
from pathlib import Path

args = argparse.ArgumentParser(description="Build QEMU command for a given test file and machine.")
args.add_argument("--file", "-f", type=str, help="Path to the firmware file.")
args.add_argument(
    "--compile",
    "-c",
    nargs="+",
    help="Compile one or more C source files before running.",
)
args.add_argument("--machine", "-m", default="mps2-an505", type=str, help="QEMU machine type.")
args.add_argument("--gdb", action="store_true", help="Enable GDB debugging.")
args.add_argument("--gcc", type=str, help="Path to the GCC compiler to use.")
args.add_argument("--cflags", type=str, help="Additional CFLAGS (e.g. -O0, -O2, -Os, -Og).")
args, _ = args.parse_known_args()

def main():
    file = None
    if args.compile:
        sources = [Path(p).resolve() for p in args.compile]
        compiler_kwargs = {}
        if args.gcc:
            print(f"Using custom compiler: {args.gcc}")
            compiler_kwargs["compiler"] = args.gcc
        if args.cflags:
            print(f"Using CFLAGS: {args.cflags}")
            compiler_kwargs["cflags"] = args.cflags
        result = compile_testcase(sources, args.machine, **compiler_kwargs)
        if not result.success:
            print(f"Compilation failed:\n{result.error}", file=sys.stderr)
            sys.exit(1)
        file = result.elf_file
    if file is None:
        file = args.file
    # Send harness diagnostics to stderr so stdout stays comparable to .expect
    print(f"Running QEMU with file: {file}", file=sys.stderr)
    qemu_command = build_qemu_command(args.machine, file)
    if args.gdb:
        qemu_command += " -s -S"
    subprocess.run(qemu_command, shell=True)

if __name__ == "__main__":
    main()