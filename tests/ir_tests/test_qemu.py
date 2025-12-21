import pytest
from pathlib import Path
from qemu_run import run_test

MACHINE = "mps2-an505"
CURRENT_DIR = Path(__file__).parent

# Add test files here - each must have a corresponding .expect file
TEST_FILES = [
    ("01_hello_world.c", 34),
    ("20_op_add.c", 0),
    ("30_function_call.c", 30),
    ("40_if.c", 0),
    ("../tests2/00_assignment.c", 0),
    ("../tests2/01_comment.c", 0),
    ("../tests2/02_printf.c", 0),
]

def load_expect_file(test_name):
    """Load and return lines from .expect file and expected exit code"""
    test_file = Path(test_name)
    expect_file = CURRENT_DIR / f"{test_file.parent}/{test_file.stem}.expect"
    if not expect_file.exists():
        raise FileNotFoundError(f"Expect file not found: {expect_file}")

    lines = []

    with open(expect_file, "r") as f:
        for line in f:
            stripped = line.rstrip('\n')
            lines.append(stripped)

    return lines



@pytest.mark.parametrize("test_file,expected_exit_code", TEST_FILES, ids=[Path(f[0]).stem for f in TEST_FILES])
def test_qemu_execution(test_file, expected_exit_code):
    if test_file is None:
        pytest.fail("test_file is None")

    print(f"Running test: {test_file}")
    expected_lines = load_expect_file(test_file)
    sut = run_test(test_file, MACHINE)

    try:
        for line in expected_lines:
            if not line is None:
                sut.expect(line, timeout=1)

        sut.wait()
        assert sut.exitstatus == expected_exit_code, f"Expected exit code {expected_exit_code}, got {sut.exitstatus}"

        sut.logfile.close()
    except Exception as e:
        # Save output log on failure
        sut.logfile.close()
        raise AssertionError(f"Test failed for {test_file}: {e}") from e

