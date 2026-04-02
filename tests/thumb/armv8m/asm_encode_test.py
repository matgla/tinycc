#!/usr/bin/env python3
"""
Assembly encoding tester for ARMv8-M Thumb-2 instructions.

Assembles a simple .S file using arm-none-eabi-as, then uses
arm-none-eabi-objdump to produce hex dumps and disassembly for
verification.

Usage:
    python asm_encode_test.py <arch> [fptype] [extensions...]

Examples:
    python asm_encode_test.py armv8-m.main
    python asm_encode_test.py armv8-m.main+dsp fpv5-sp-d16
    python asm_encode_test.py armv8-m.main+nod3 fpv4-sp-d16 nortc
"""

import argparse
import subprocess
import sys
import tempfile
import os
from pathlib import Path


SAMPLE_ASM = """\
.syntax unified
.thumb

.global _start
_start:
    nop
    mov r0, #1
    add r1, r2, #3
    sub r3, r4, #0x10
    and r5, r6, #0xFF
    orr r7, r8, #0x10
    eor r9, r10, #0x55
    lsl r0, r1, #2
    lsr r0, r1, #3
    asr r0, r1, #4
    b target
target:
    bx lr
"""


def run_cmd(cmd, description="", timeout=30):
    print(f"  $ {' '.join(cmd)}")
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        print(f"\n[ERROR] Command timed out after {timeout}s:")
        sys.exit(1)
    if result.returncode != 0:
        print(f"\n[ERROR] {description or 'Command failed'}:")
        if result.stderr.strip():
            for line in result.stderr.strip().splitlines():
                print(f"    {line}")
        sys.exit(1)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(
        description="Test ARMv8-M Thumb-2 assembly encoding",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  %(prog)s armv8-m.main
  %(prog)s armv8-m.main+dsp fpv5-sp-d16
  %(prog)s cortex-m33 fpv5-sp-d16 nortc

Architecture extensions are appended to the base arch string with '+'."""
    )
    parser.add_argument(
        "arch",
        help="Base architecture (e.g. armv8-m.main, cortex-m33)",
    )
    parser.add_argument(
        "fptype",
        nargs="?",
        default=None,
        help="FPU type (e.g. fpv5-sp-d16, fpv4-sp-d16, fp-armv8)",
    )
    parser.add_argument(
        "extensions",
        nargs="*",
        default=[],
        help="Architecture extensions appended with '+' (e.g. nod3c nortc)",
    )

    args = parser.parse_args()

    # Build -march string
    arch_parts = [args.arch] + args.extensions
    march = "+".join(arch_parts)

    # Build flags
    mfloat_abi = "hard" if args.fptype else "soft"
    mfpu = f"-mfpu={args.fptype}" if args.fptype else "-mfloat-abi=soft"

    print(f"Architecture : {march}")
    print(f"FPU          : {args.fptype or '(none)'}")
    print(f"Float ABI    : {mfloat_abi}")
    print()

    # Locate toolchain binaries
    as_cmd = ["arm-none-eabi-as"]
    objdump_cmd = ["arm-none-eabi-objdump"]

    for cmd in [as_cmd, objdump_cmd]:
        try:
            run_cmd(cmd + ["--version"], f"{cmd[0]} not found")
        except SystemExit:
            print(f"\n[ERROR] {cmd[0]} not found in PATH. Install gcc-arm-none-eabi.")
            sys.exit(1)

    # Write sample assembly to temp file
    with tempfile.NamedTemporaryFile(mode="w", suffix=".S", delete=False) as f:
        asm_file = f.name
        f.write(SAMPLE_ASM)

    obj_file = asm_file.replace(".S", ".o")
    hex_file = asm_file.replace(".S", "_hex.txt")
    disasm_file = asm_file.replace(".S", "_disasm.txt")

    try:
        # Assemble
        print("[1] Assembling with arm-none-eabi-as ...")
        as_args = [asm_file, "-o", obj_file, "--warn"]
        if args.fptype:
            as_args += [f"-march={march}", mfpu, f"-mfloat-abi={mfloat_abi}"]
        else:
            as_args += [f"-march={march}", f"-mfloat-abi=soft"]
        run_cmd(as_cmd + as_args, description="Assembly failed")
        print("  Assembly succeeded.\n")

        # Hex dump (raw object file bytes)
        print("[2] Raw hex dump of object file ...")
        hex_output = run_cmd(
            ["xxd", obj_file],
            description="xxd not found, skipping raw hex dump",
        )
        with open(hex_file, "w") as f:
            f.write(hex_output)

        # Disassembly from object file
        print("[3] Disassembling object file ...")
        disasm_output = run_cmd(
            objdump_cmd + [
                "-d",
                "-marm",
                f"-Mforce-thumb",
                obj_file,
            ],
            description="Disassembly failed",
        )
        with open(disasm_file, "w") as f:
            f.write(disasm_output)

        print()
        print("=" * 72)
        print("DISASSEMBLY:")
        print("=" * 72)
        print(disasm_output)

        # Also show hex dump of just the .text section via objdump -s
        print("=" * 72)
        print("HEX DUMP (.text section):")
        print("=" * 72)
        hex_section = run_cmd(
            objdump_cmd + [
                "-s",
                "-j", ".text",
                "-marm",
                "-Mforce-thumb",
                obj_file,
            ],
            description="Hex dump failed",
        )
        print(hex_section)

        # Also produce a plain hex stream of the .text bytes for easy comparison
        print("=" * 72)
        print("PLAIN HEX STREAM (.text only):")
        print("=" * 72)

        # Extract just the hex bytes from objdump output (already captured above)
        import re
        hex_lines = []
        for line in hex_section.splitlines():
            stripped = line.strip()
            if not stripped or ':' in stripped and '@' not in stripped:
                continue
            # Match lines like "00bf4ff0 010002f1 ..."
            match = re.match(r'^([0-9a-f]+(?:\s+[0-9a-f]+)*)', stripped)
            if match:
                hex_lines.append(match.group(1).replace(' ', ''))
        print("".join(hex_lines).upper())

    finally:
        # Cleanup temp files (keep output files for inspection)
        os.unlink(asm_file)


if __name__ == "__main__":
    main()
