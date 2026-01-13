import pexpect
import subprocess

from pathlib import Path

CURRENT_DIR = Path(__file__).parent

was_cleaned = False


def _as_file_list(test_file):
    if isinstance(test_file, (list, tuple)):
        return list(test_file)
    return [test_file]


def _primary_file(test_file):
    files = _as_file_list(test_file)
    if not files:
        raise ValueError("test_file list is empty")
    return files[0]

def get_test_output_file(test_name):
    primary = _primary_file(test_name)
    return f"{CURRENT_DIR}/build/{Path(primary).stem}.elf"

def build_make_command(test_file, machine, compiler, cflags=None):
    make_dir = CURRENT_DIR / 'qemu' / machine
    test_files = [str(f) for f in _as_file_list(test_file)]
    test_files_value = " ".join(test_files)
    cmd = [
        "make",
        "-C",
        str(make_dir),
        f"OUTPUT={CURRENT_DIR}/build",
        f"TEST_FILES={test_files_value}",
        f"CC={compiler}",
        f"TARGET={get_test_output_file(test_file)}",
    ]
    if cflags:
        cmd.append(f"EXTRA_CFLAGS={cflags}")
    return cmd

def build_qemu_command(machine, kernel_file, args=None):
    cmd = f'qemu-system-arm -machine {machine} -nographic -semihosting -kernel {kernel_file}'
    if args:
        cmd += ' -append "' + ' '.join(args) + '"'
    return cmd

def compile_testcase(test_file, machine, compiler=f"{CURRENT_DIR}/../../armv8m-tcc", cflags=None):
    global was_cleaned
    make_command = build_make_command(test_file, machine, compiler, cflags)
    if not was_cleaned:
        result = subprocess.run(make_command + ["clean"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode != 0:
            raise RuntimeError(f"Clean failed with exit code {result.returncode}")
        was_cleaned = True
    result = subprocess.run(make_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        if result.stdout != None:
            print(result.stdout.decode())
        if result.stderr != None:
            print(result.stderr.decode())
        raise RuntimeError(f"Build failed with exit code {result.returncode}")
    output_lines = result.stdout.decode().splitlines() if result.stdout else []
    output_lines += result.stderr.decode().splitlines() if result.stderr else []
    result = get_test_output_file(test_file)
    return result, output_lines

def prepare_test(machine, kernel_file, args=None):
    qemu_command = build_qemu_command(machine, kernel_file, args)
    # Use a wide pseudo-terminal so long lines (e.g. separators) aren't wrapped.
    # Wrapped lines confuse the pytest pexpect-based matcher and lead to EOF mismatches
    # even when the program output is correct.
    sut = pexpect.spawn(qemu_command)
    # rows, cols
    sut.setwinsize(200, 1000)
    return sut

def run_test(test_file, machine, args=None, cflags=None):
    primary = _primary_file(test_file)
    test_name = Path(primary).stem

    test_files = [CURRENT_DIR / Path(f) for f in _as_file_list(test_file)]
    output_file, loglines = compile_testcase(test_files, machine, cflags=cflags)
    sut = prepare_test(machine, output_file, args)

    # Enable logging to file using test name
    log_file = open(f"{CURRENT_DIR}/build/{test_name}_output.log", "wb")
    sut.logfile = log_file
    return sut, loglines

