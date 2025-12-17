import pexpect
import subprocess

from pathlib import Path

CURRENT_DIR = Path(__file__).parent

def get_test_output_file(test_name):
    return f"{CURRENT_DIR}/build/{Path(test_name).stem}.elf"

def build_make_command(test_file, machine):
    return f'make -C qemu/{machine} OUTPUT={CURRENT_DIR}/build TEST_FILES={test_file} CC={CURRENT_DIR}/../../armv8m-tcc TARGET={get_test_output_file(test_file)}'

def build_qemu_command(machine, kernel_file):
    return f'qemu-system-arm -machine {machine} -nographic -semihosting -kernel {kernel_file}'

def compile_testcase(test_file, machine):
    make_command = build_make_command(test_file, machine)
    result = subprocess.run(make_command + " clean", shell=True)
    if result.returncode != 0:
        raise RuntimeError(f"Clean failed with exit code {result.returncode}")
    result = subprocess.run(make_command, shell=True)
    if result.returncode != 0:
        raise RuntimeError(f"Build failed with exit code {result.returncode}")
    return get_test_output_file(test_file)

def prepare_test(machine, kernel_file):
    qemu_command = build_qemu_command(machine, kernel_file)
    return pexpect.spawn(qemu_command)

def run_test(test_file, machine, timeout=1):
    test_name = Path(test_file).stem
    output_file = compile_testcase(CURRENT_DIR / test_file, machine)
    sut = prepare_test(machine, output_file)

    # Enable logging to file using test name
    log_file = open(f"{CURRENT_DIR}/build/{test_name}_output.log", "wb")
    sut.logfile = log_file
    return sut

