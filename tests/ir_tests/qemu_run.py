import pexpect
import subprocess

from pathlib import Path

CURRENT_DIR = Path(__file__).parent

was_cleaned = False

def get_test_output_file(test_name):
    return f"{CURRENT_DIR}/build/{Path(test_name).stem}.elf"

def build_make_command(test_file, machine, compiler):
    return f'make -C qemu/{machine} OUTPUT={CURRENT_DIR}/build TEST_FILES={test_file} CC={compiler} TARGET={get_test_output_file(test_file)}'

def build_qemu_command(machine, kernel_file):
    return f'qemu-system-arm -machine {machine} -nographic -semihosting -kernel {kernel_file}'

def compile_testcase(test_file, machine, compiler=f"{CURRENT_DIR}/../../armv8m-tcc"):
    global was_cleaned
    make_command = build_make_command(test_file, machine, compiler)
    if not was_cleaned:
        result = subprocess.run(make_command + " clean", shell=True)
        if result.returncode != 0:
            raise RuntimeError(f"Clean failed with exit code {result.returncode}")
        was_cleaned = True
    result = subprocess.run(make_command, shell=True)#, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        print(result.stdout.decode())
        print(result.stderr.decode())
        raise RuntimeError(f"Build failed with exit code {result.returncode}")
    output_lines = result.stdout.decode().splitlines() if result.stdout else []
    output_lines += result.stderr.decode().splitlines() if result.stderr else []
    result = get_test_output_file(test_file)
    return result, output_lines

def prepare_test(machine, kernel_file):
    qemu_command = build_qemu_command(machine, kernel_file)
    return pexpect.spawn(qemu_command)

def run_test(test_file, machine):
    test_name = Path(test_file).stem
    output_file, loglines = compile_testcase(CURRENT_DIR / test_file, machine)
    sut = prepare_test(machine, output_file)

    # Enable logging to file using test name
    log_file = open(f"{CURRENT_DIR}/build/{test_name}_output.log", "wb")
    sut.logfile = log_file
    return sut, loglines

