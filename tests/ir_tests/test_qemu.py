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
    ("50_simple_struct.c", 0),
    ("60_landor.c", 0),
    # ("61_simple_or.c", 0),
    # ("../tests2/00_assignment.c", 0),
    # ("../tests2/01_comment.c", 0),
    # ("../tests2/02_printf.c", 0),
    # ("../tests2/03_struct.c", 0),
    # ("../tests2/04_for.c", 0),
    # ("../tests2/05_array.c", 0),
    # ("../tests2/06_case.c", 0),
    # ("../tests2/07_function.c", 0),
    # ("../tests2/08_while.c", 0),
    # ("../tests2/09_do_while.c", 0),
    # ("../tests2/10_pointer.c", 0),
    # ("../tests2/11_precedence.c", 0),
    # ("../tests2/12_hashdefine.c", 0),
    # ("../tests2/13_integer_literals.c", 0),
    # ("../tests2/14_if.c", 0),
    # ("../tests2/15_recursion.c", 0),
    # ("../tests2/16_nesting.c", 0),
    # ("../tests2/17_enum.c", 0),
    # ("../tests2/18_include.c", 0),
    # ("../tests2/19_pointer_arithmetic.c", 0),
    # ("../tests2/20_pointer_comparison.c", 0),
    # ("../tests2/21_char_array.c", 0),
    # ("../tests2/22_floating_point.c", 0),
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

    expected_lines = load_expect_file(test_file)
    sut, loglines = run_test(test_file, MACHINE)
    # remove expected compiler output
    compiler_verified = False
    for line in expected_lines:
        if compiler_verified:
            break
        for logline in loglines:
            if line in logline:
                expected_lines = [l for l in expected_lines if l != line]
                compiler_verified = True
                break


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

