from qemu_run import build_qemu_command, compile_testcase

import argparse
import subprocess
from pathlib import Path

args = argparse.ArgumentParser(description="Build QEMU command for a given test file and machine.")
args.add_argument("--file", "-f", type=str, help="Path to the firmware file.")
args.add_argument("--compile", "-c", type=str, help="Compile the test file before running.")
args.add_argument("--machine", "-m", default="mps2-an505", type=str, help="QEMU machine type.")
args.add_argument("--gdb", action="store_true", help="Enable GDB debugging.")
args, _ = args.parse_known_args()

def main():
    file = None
    if args.compile:
        file, _ = compile_testcase(Path(args.compile).resolve(), args.machine)
    if file is None:
        file = args.file
    print(f"Running QEMU with file: {file}")
    qemu_command = build_qemu_command(args.machine, file)
    if args.gdb:
        qemu_command += " -s -S"
    subprocess.run(qemu_command, shell=True)

if __name__ == "__main__":
    main()