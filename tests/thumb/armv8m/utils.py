import subprocess
import os
import re
from pathlib import Path

def prepare_expect(filepath):
    """
    Compiles the assembly code at the given filepath using the ARM toolchain.
    """
    try:
        compiler = os.getenv("TEST_COMPARE_CC", None)
        output_dir = (Path(filepath).parent / "expected").resolve()
        output_file = output_dir / (Path(filepath).stem)
        output_file_gcc = output_dir / (Path(filepath).stem + "_gcc")
        if not os.path.exists(output_dir):
            os.makedirs(output_dir)
        assert compiler is not None, "TEST_COMPARE_CC environment variable must be set to the ARM compiler path."
        _ = subprocess.run(
            [compiler, filepath, "-march=armv8-m.main+dsp", "-mfpu=fpv5-sp-d16", "-mfloat-abi=hard", "-nostdlib", "-Wl,-Ttext=0x0", "-o", output_file_gcc],
            check=True,
            capture_output=True,
            text=True
        )

        objcopy = os.getenv("TEST_OBJCOPY", None)
        _ = subprocess.run(
            [objcopy, "--only-section=.text", output_file_gcc, output_file]
        )

        return output_file
    except subprocess.CalledProcessError as e:
        print(f"Compilation failed: {e.stderr}")
        raise e


def compile_code(filepath):
    """
    Compiles the assembly code at the given filepath using the ARM toolchain.
    """
    try:
        compiler = os.getenv("TEST_CC", None)
        print(filepath)
        output_dir = (Path(filepath).parent / "build").resolve()
        output_file = output_dir / (Path(filepath).stem)
        if not os.path.exists(output_dir):
            os.makedirs(output_dir)
        assert compiler is not None, "TEST_CC environment variable must be set to the ARM compiler path."
        result = subprocess.run(
            [compiler, filepath, "-g", "-nodefaultlibs", "-Wl,-oformat=elf32-littlearm", "-o", output_file],
            check=True,
            capture_output=True,
            text=True
        )
        print(f"Compilation successful: {result.stdout}")
        return output_file
    except subprocess.CalledProcessError as e:
        print(f"Compilation failed: {e.stderr}")
        raise e

def disassemble_code(filepath):
    """
    Disassembles the compiled object file using the ARM toolchain.
    """
    try:
        disassembler = os.getenv("TEST_OBJDUMP", None)
        assert disassembler is not None, "TEST_OBJDUMP environment variable must be set to the ARM disassembler path."
        result = subprocess.run(
            [disassembler, "-D", "-marm", "-marmv8-m.main", "-Mforce-thumb", filepath],
            check=True,
            capture_output=True,
            text=True
        )
        return result.stdout
    except subprocess.CalledProcessError as e:
        print(f"Disassembly failed: {e.stderr}")
        raise e

def cleanup_dissambly(disassembly):
    tmp = []
    for line in disassembly:
        line = line.strip()
        if re.match(r'^[0-9A-Fa-f]+:', line):
            line = line.split("\t")
            line = [s.strip() for s in line]
            tmp.append(line)
    output = []
    for line in tmp:
        if len(line) == 0:
            return output

        output.append(line)

    return output

def perform_test_for_file(file):
    output_file = compile_code(file)
    disassembly_sut = disassemble_code(output_file).splitlines()
    expected_file = prepare_expect(file)
    disassembly_expected = disassemble_code(expected_file).splitlines()
    verify_disassembly(disassembly_sut, disassembly_expected)

def verify_disassembly(disassembly_sut, disassembly_expected):
    sut = cleanup_dissambly(disassembly_sut)
    expected = cleanup_dissambly(disassembly_expected)

    assert len(expected) > 0, "Expected disassembly is empty"

    for i in range(len(expected)):
        assert sut[i][1] == expected[i][1], f"Mismatch at line {i}: {sut[i]} != {expected[i]}"
