import pytest
from pathlib import Path
from qemu_run import run_test

MACHINE = "mps2-an505"
CURRENT_DIR = Path(__file__).parent

# Add test files here - each must have a corresponding .expect file
TEST_FILES = [
    "01_hello_world.c",
    "20_op_add.c"
]

def load_expect_file(test_name):
    """Load and return lines from .expect file"""
    expect_file = CURRENT_DIR / f"{Path(test_name).stem}.expect"
    if not expect_file.exists():
        raise FileNotFoundError(f"Expect file not found: {expect_file}")
    with open(expect_file, "r") as f:
        return [line.rstrip('\n') for line in f if line.strip()]

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
    expected_lines = load_expect_file(test_file)
    sut = qemu_runner(test_file)

    for line in expected_lines:
        sut.expect(line, timeout=1)

