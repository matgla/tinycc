import pytest
import re
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
    ("61_simple_or.c", 0),
    ("90_global_array_assignment.c", 0),
    ("bug_swap.c", 0),
    ("bug_partition.c", 0),
    ("91_const_propagation.c", 0),
    ("92_loop_invariant.c", 0),
    ("93_chained_arithmetic.c", 0),
    ("94_copy_propagation.c", 0),
    ("95_cse.c", 0),
    ("test_ge_operator.c", 0),
    ("97_void_call_noargs.c", 0),
    ("98_call_over32_args.c", 0),
    ("99_struct_init_from_struct.c", 0),
    # ("test_llong_relops.c", 0),

    # ("test_llong_add_signed.c", 0),
    # ("test_llong_add_unsigned.c", 0),
    # ("test_llong_load_signed.c", 0),
    # ("test_llong_load_unsigned.c", 0),
    # ("test_llong_mul_signed.c", 0),
    # ("test_llong_mul_unsigned.c", 0),
    # ("test_llong_div_signed.c", 0),
    # ("test_llong_div_unsigned.c", 0),
    # ("test_llong_mod_signed.c", 0),
    # ("test_llong_mod_unsigned.c", 0),

    ("../tests2/00_assignment.c", 0),
    ("../tests2/01_comment.c", 0),
    ("../tests2/02_printf.c", 0),
    ("../tests2/03_struct.c", 0),
    ("../tests2/04_for.c", 0),
    ("../tests2/05_array.c", 0),
    ("../tests2/06_case.c", 0),
    ("../tests2/07_function.c", 0),
    ("../tests2/08_while.c", 0),
    ("../tests2/09_do_while.c", 0),
    ("../tests2/10_pointer.c", 0),
    ("../tests2/11_precedence.c", 0),
    ("../tests2/12_hashdefine.c", 0),
    ("../tests2/13_integer_literals.c", 0),
    ("../tests2/14_if.c", 0),
    ("../tests2/15_recursion.c", 0),
    ("../tests2/16_nesting.c", 0),
    ("../tests2/17_enum.c", 0),
    ("../tests2/18_include.c", 0),
    ("../tests2/19_pointer_arithmetic.c", 0),
    ("../tests2/20_pointer_comparison.c", 0),
    ("../tests2/21_char_array.c", 0),

    ("../tests2/25_quicksort.c", 0),
    ("../tests2/26_character_constants.c", 0),
    ("../tests2/27_sizeof.c", 0),
    ("../tests2/28_strings.c", 0),
    ("../tests2/29_array_address.c", 0),
    ("../tests2/30_hanoi.c", 0),
    ("../tests2/33_ternary_op.c", 0),
    ("../tests2/34_array_assignment.c", 0),
    ("../tests2/35_sizeof.c", 0),
    ("../tests2/36_array_initialisers.c", 0),
    ("../tests2/37_sprintf.c", 0),
    ("../tests2/38_multiple_array_index.c", 0),
    ("../tests2/39_typedef.c", 0),
    # ("../tests2/40_stdio.c", 0), # requires runtime environment
    ("../tests2/41_hashif.c", 0),
    ("../tests2/42_function_pointer.c", 0),
    ("../tests2/43_void_param.c", 0),
    ("../tests2/44_scoped_declarations.c", 0),
    ("../tests2/45_empty_for.c", 0),
    # ("../tests2/46_grep.c", 0), # runtime environment needed
    ("../tests2/47_switch_return.c", 0),
    ("../tests2/48_nested_break.c", 0),
    ("../tests2/50_logical_second_arg.c", 0),
    ("../tests2/51_static.c", 0),
    ("../tests2/52_unnamed_enum.c", 0),
    ("../tests2/54_goto.c", 0),
    # ("../tests2/55_lshift_type.c", 0),
    # ("../tests2/60_errors_and_warnings.c", 0), # separate test with tags
    ("../tests2/61_integers.c", 0),
    ("../tests2/64_macro_nesting.c", 0),
    ("../tests2/67_macro_concat.c", 0),
    ("../tests2/71_macro_empty_arg.c", 0),
    ("../tests2/72_long_long_constant.c", 0),
    ("../tests2/75_array_in_struct_init.c", 0),
    ("../tests2/76_dollars_in_identifiers.c", 0),
    ("../tests2/77_push_pop_macro.c", 0),
    ("../tests2/78_vla_label.c", 0),
    ("../tests2/79_vla_continue.c", 0),
    ("../tests2/80_flexarray.c", 0),
    ("../tests2/81_types.c", 0),
    ("../tests2/82_attribs_position.c", 0),
    ("../tests2/85_asm-outside-function.c", 0),
    ("../tests2/86_memory-model.c", 0),
    ("../tests2/87_dead_code.c", 0),
    ("../tests2/88_codeopt.c", 0),
    ("../tests2/89_nocode_wanted.c", 0),
    ("../tests2/90_struct-init.c", 0),
    ("../tests2/91_ptr_longlong_arith32.c", 0),
    ("../tests2/92_enum_bitfield.c", 0),
    ("../tests2/93_integer_promotion.c", 0),
    # ("../tests2/95_bitfields.c", 0),
    # ("../tests2/95_bitfields_ms.c", 0),
    # ("../tests2/96_nodata_wanted.c", 0),
    ("../tests2/97_utf8_string_literal.c", 0),
    # ("../tests2/98_al_ax_extend.c", 0),
    # ("../tests2/99_fastcall.c", 0),
    ("../tests2/100_c99array-decls.c", 0),
    # ("../tests2/101_cleanup.c", 0),
    ("../tests2/102_alignas.c", 0),
    ("../tests2/103_implicit_memmove.c", 0),
    (["../tests2/104_inline.c", "../tests2/104+_inline.c"], 0),
    ("../tests2/105_local_extern.c", 0),
    # ("../tests2/106_versym.c", 0),
    ("../tests2/108_constructor.c", 0),
    # ("../tests2/112_backtrace.c", 0),
    # ("../tests2/113_btdll.c", 0),
    # ("../tests2/114_bound_signal.c", 0),
    # ("../tests2/115_bound_setjmp.c", 0),
    # ("../tests2/116_bound_setjmp2.c", 0),
    # ("../tests2/117_builtins.c", 0),
    # ("../tests2/118_switch.c", 0),
    (["../tests2/120_alias.c", "../tests2/120+_alias.c"], 0),
    ("../tests2/122_vla_reuse.c", 0),
    ("../tests2/123_vla_bug.c", 0),
    # ("../tests2/124_atomic_counter.c", 0),
    # ("../tests2/125_atomic_misc.c", 0),
    # ("../tests2/126_bound_global.c", 0),
    # ("../tests2/127_asm_goto.c", 0),
    # ("../tests2/128_run_atexit.c", 0),
    ("../tests2/129_scopes.c", 0),
    ("../tests2/130_large_argument.c", 0),
    ("../tests2/133_string_concat.c", 0),
    ("../tests2/135_func_arg_struct_compare.c", 0),
]

FLOAT_TEST_FILES = [
    ("../tests2/22_floating_point.c", 0),
    ("../tests2/23_type_coercion.c", 0),
    ("../tests2/24_math_library.c", 0),
    ("../tests2/32_led.c", 0),
    ("../tests2/49_bracket_evaluation.c", 0),
    ("../tests2/70_floating_point_literals.c", 0),
    ("../tests2/73_arm64.c", 0),
    ("../tests2/83_utf8_in_identifiers.c", 0),
    ("../tests2/84_hex-float.c", 0),
    ("../tests2/94_generic.c", 0),
    ("../tests2/107_stack_safe.c", 0),
    ("../tests2/109_float_struct_calling.c", 0),
    ("../tests2/110_average.c", 0),
    ("../tests2/111_conversion.c", 0),
    ("../tests2/119_random_stuff.c", 0),
    ("../tests2/121_struct_return.c", 0),
    ("../tests2/131_return_struct_in_reg.c", 0),
    ("../tests2/132_bound_test.c", 0),
    ("../tests2/134_double_to_signed.c", 0),

]

TEST_FILES_WITH_ARGS = [
    ("../tests2/31_args.c", ["arg1", "arg2", "arg3", "arg4", "arg5"], 0),
]


def _primary_test_file(test_file):
    return test_file[0] if isinstance(test_file, (list, tuple)) else test_file


def _test_id(test_file):
    return Path(_primary_test_file(test_file)).stem

def load_expect_file(test_name):
    """Load and return lines from .expect file and expected exit code"""
    test_file = Path(_primary_test_file(test_name))
    expect_file = CURRENT_DIR / f"{test_file.parent}/{test_file.stem}.expect"
    if not expect_file.exists():
        raise FileNotFoundError(f"Expect file not found: {expect_file}")

    lines = []

    with open(expect_file, "r") as f:
        for line in f:
            stripped = line.rstrip('\n')
            lines.append(stripped)

    return lines


def _strip_compiler_output(expected_lines, loglines):
    """Remove compiler output from the expectation list."""
    sanitized = expected_lines.copy()
    compiler_verified = False
    for line in expected_lines:
        if compiler_verified:
            break
        for logline in loglines:
            if line in logline:
                sanitized = [l for l in sanitized if l != line]
                compiler_verified = True
                break
    return sanitized


def _escape_regex(line):
    """Escape regex special characters in a line so it's treated literally."""
    return re.escape(line)


def _run_qemu_test(test_file, expected_exit_code, args=None):
    expected_lines = load_expect_file(test_file)
    sut, loglines = run_test(test_file, MACHINE, args)
    expected_lines = _strip_compiler_output(expected_lines, loglines)
    try:
        for line in expected_lines:
            if line is not None:
                sut.expect(_escape_regex(line), timeout=1)
        sut.wait()
        assert sut.exitstatus == expected_exit_code, f"Expected exit code {expected_exit_code}, got {sut.exitstatus}"
    except Exception as e:
        raise AssertionError(f"Test failed for {test_file}: {e}") from e
    finally:
        sut.logfile.close()



@pytest.mark.parametrize("test_file,expected_exit_code", TEST_FILES, ids=[_test_id(f[0]) for f in TEST_FILES])
def test_qemu_execution(test_file, expected_exit_code):
    if test_file is None:
        pytest.fail("test_file is None")

    _run_qemu_test(test_file, expected_exit_code)


@pytest.mark.parametrize(
    "test_file,args,expected_exit_code",
    TEST_FILES_WITH_ARGS,
    ids=[_test_id(f[0]) for f in TEST_FILES_WITH_ARGS],
)
def test_qemu_execution_with_args(test_file, args, expected_exit_code):
    if test_file is None:
        pytest.fail("test_file is None")

    _run_qemu_test(test_file, expected_exit_code, args=args)

