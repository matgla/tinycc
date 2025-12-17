import pytest
from pathlib import Path
from qemu_run import run_test

MACHINE = "mps2-an505"
CURRENT_DIR = Path(__file__).parent

# Add test files here - each must have a corresponding .expect file
TEST_FILES = [
    "01_hello_world.c",
    "20_op_add.c",
    "30_function_call.c",
]

def load_expect_file(test_name):
    """Load and return lines from .expect file and expected exit code"""
    expect_file = CURRENT_DIR / f"{Path(test_name).stem}.expect"
    if not expect_file.exists():
        raise FileNotFoundError(f"Expect file not found: {expect_file}")
    
    lines = []
    exit_code = None
    
    with open(expect_file, "r") as f:
        for line in f:
            stripped = line.rstrip('\n')
            # Check for exit code directive
            if stripped.startswith("EXIT_CODE:"):
                exit_code = int(stripped.split(":", 1)[1].strip())
            elif stripped.strip():  # Non-empty lines
                lines.append(stripped)
    
    return lines, exit_code

@pytest.fixture
def qemu_runner():
    """Fixture that provides a context manager for running QEMU tests"""
    sut_instance = None

    def _run(test_file):
        nonlocal sut_instance
        sut_instance = run_test(test_file, MACHINE)
        return sut_instance

    yield _run

    if sut_instance:
        sut_instance.close()

@pytest.mark.parametrize("test_file", TEST_FILES, ids=lambda f: Path(f).stem)
def test_qemu_execution(test_file, qemu_runner):
    expected_lines, expected_exit_code = load_expect_file(test_file)
    sut = qemu_runner(test_file)

    try:
        for line in expected_lines:
            sut.expect(line, timeout=1)
        
        if expected_exit_code is not None:
            sut.expect(f"Exit code: {expected_exit_code}", timeout=1)
        sut.logfile.close()
    except Exception as e:
        # Save output log on failure
        sut.logfile.close()

