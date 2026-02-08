import pytest
import re
from pathlib import Path
from qemu_run import run_test, compile_testcase, CompileConfig, prepare_test


# When expected output contains floating point literals, match numerically and
# compare with a tolerance instead of exact string match.
# This is useful because some embedded printf implementations can differ in
# rounding/truncation behaviour for %f formatting.
_FLOAT_RE = r"[-+]?(?:\d+\.\d*|\d*\.\d+)(?:[eE][-+]?\d+)?"
_FLOAT_EXPECT_LINE_RE = re.compile(rf"^(?P<prefix>.*?=)(?P<value>{_FLOAT_RE})$")
_FLOAT_CAPTURE_RE = rf"({_FLOAT_RE})"


def _expect_line(sut, expected_line: str, *, timeout: int = 1, float_tol: float = 1e-5):
    """Expect a line from QEMU output.

    If the expected line ends with a float literal (e.g. "sum=3.500000"),
    capture the actual float and compare within tolerance.
    """
    if expected_line is None:
        return

    float_matches = list(re.finditer(_FLOAT_RE, expected_line))
    if float_matches:
        # Build a regex that treats all non-float parts literally, and captures
        # each float. Then compare each captured float numerically.
        parts = []
        expected_values = []
        last_end = 0
        for fm in float_matches:
            parts.append(re.escape(expected_line[last_end:fm.start()]))
            parts.append(_FLOAT_CAPTURE_RE)
            expected_values.append(float(fm.group(0)))
            last_end = fm.end()
        parts.append(re.escape(expected_line[last_end:]))
        pattern = "".join(parts)

        sut.expect(pattern, timeout=timeout)
        actual_values = [float(sut.match.group(i + 1)) for i in range(len(expected_values))]
        for expected_value, actual_value in zip(expected_values, actual_values):
            if abs(actual_value - expected_value) > float_tol:
                raise AssertionError(
                    f"Float output mismatch: expected {expected_value} got {actual_value} (tol={float_tol})"
                )
        return

    sut.expect(_escape_regex(expected_line), timeout=timeout)

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
    ("bug_llong_const.c", 0),
    ("bug_mul_by_const.c", 0),
    ("bug_mul_compound.c", 0),
    ("bug_ull_mul10_loop.c", 0),
    ("bug_ull_mul10_once.c", 0),
    ("bug_ll_mul10_switch_min.c", 0),
    # ("bug_ternary_string.c", 0),  # Nested ternary with string literals
    # ("bug_return_else_string.c", 0),  # Return string from else block
    ("test_cleanup_double.c", 0),
    ("91_const_propagation.c", 0),
    ("92_loop_invariant.c", 0),
    ("93_chained_arithmetic.c", 0),
    ("94_copy_propagation.c", 0),
    ("95_cse.c", 0),
    ("95_const_branch_fold.c", 0),
    ("96_const_cmp_fold_vreg.c", 0),
    ("97_loop_const_expr.c", 0),
    ("98_value_tracking.c", 0),
    ("test_fp_offset_cache.c", 0),
    ("test_ge_operator.c", 0),
    ("test_mla_fusion.c", 0),
    ("test_offset_addressing.c", 0),
    ("97_void_call_noargs.c", 0),
    ("98_call_over32_args.c", 0),
    ("99_struct_init_from_struct.c", 0),
    ("test_struct_pass_by_value.c", 0),
    ("test_struct_return.c", 0),
    ("test_llong_relops.c", 0),
    ("test_double_printf_ops.c", 0),
    ("test_double_printf_literals.c", 0),
    ("test_double_printf_mixed.c", 0),

    # Pure function hoisting tests (LICM optimization)
    ("100_pure_func_strlen.c", 0),
    ("101_pure_func_abs.c", 0),
    ("102_pure_func_strcmp.c", 0),
    ("103_pure_func_multiple.c", 0),
    ("104_pure_func_variant.c", 0),

    # Single-precision float tests
    ("72_float_result.c", 1),  # Returns 1 on success (non-standard convention)
    ("73_float_ops.c", 1),     # Returns 1 on success

    # AEABI soft-float regressions (bit-level tests; avoids printf %f).
    ("test_aeabi_dmul_bits.c", 0),
    ("test_f2d_bits.c", 0),
    ("test_aeabi_double_all.c", 0),

    ("test_dmul_orig_override.c", 0),

    ("test_llong_add_signed.c", 0),
    ("test_llong_add_unsigned.c", 0),
    ("test_llong_load_signed.c", 0),
    ("test_llong_load_unsigned.c", 0),
    ("test_llong_mul_signed.c", 0),
    ("test_llong_mul_unsigned.c", 0),
    ("test_llong_mul_parts.c", 0),
    ("test_llong_mul_64bit.c", 0),
    ("test_llong_mul_reg.c", 0),

    ("test_mul32wide_outparams.c", 0),
    ("test_mul64wide_compare.c", 0),
    ("test_u64_mask_bit41.c", 0),
    ("test_u64_param_split.c", 0),
    ("test_u64_shift32.c", 0),
    ("test_u64_shift_add.c", 0),
    ("test_llong_div_signed.c", 0),
    ("test_llong_div_unsigned.c", 0),
    ("test_llong_mod_signed.c", 0),
    ("test_llong_mod_unsigned.c", 0),
    ("test_llong_bitwise.c", 0),

    # Induction variable strength reduction test
    ("110_iv_strength_reduction.c", 0),

    # MiBench regression (SHA-1 miscompiled under -O1)
    ("test_mibench_sha.c", 0),

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
    ("../tests2/55_lshift_type.c", 0),
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
    # ("../tests2/95_bitfields_ms.c", 0), # MS bitfield layout
    ("../tests2/97_utf8_string_literal.c", 0),
    # ("../tests2/98_al_ax_extend.c", 0), # x86
    # ("../tests2/99_fastcall.c", 0), # x86
    ("../tests2/100_c99array-decls.c", 0),
    ("../tests2/101_cleanup.c", (105, 30)),  # Longer timeout for cleanup test
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
    ("../tests2/118_switch.c", 0),
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

    # Switch statement tests (jump table optimization)
    ("test_switch.c", 0),
    ("test_switch_simple.c", 0),
    ("test_switch_small.c", 0),  # Only 3 cases - won't trigger jump table
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

# Known TCC compiler bug reproduction tests
# These tests are expected to fail until the bugs are fixed
TCC_BUG_TEST_FILES = [
    # Bug: "load_to_dest_ir I64/F64: dest.pr1 is spilled, need IR-level handling"
    # Occurs when returning 64-bit values from functions with volatile memory access
    ("test_tcc_i64_ir_bug.c", 0),

    # Bug: Volatile register access issues with ARM DWT cycle counter
    ("test_tcc_volatile_reg.c", 0),

    # Bug: Float math loop produces incorrect result
    # TCC returns 4999/8999 instead of expected 2574 in float math calculations
    # See: bench_math.c bench_float_math() benchmark
    ("test_float_math_loop.c", 0),

    # Debug test for float operations
    ("test_float_simple_calc.c", 0),
]

TEST_FILES_WITH_ARGS = [
    ("../tests2/31_args.c", ["arg1", "arg2", "arg3", "arg4", "arg5"], 0),
]

# Tagged test files: source files where tags are auto-discovered from .expect file
# Tags are identified by [tag_name] lines in the expect file
# Each tag becomes a separate test with -Dtag_name define
TAGGED_TEST_FILES = [
    "../tests2/60_errors_and_warnings.c",
    "../tests2/95_bitfields.c",
    "../tests2/96_nodata_wanted.c",
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


def load_tagged_expect_file(test_name):
    """Load and parse a tagged .expect file.

    Returns a dict: {tag_name: {"lines": [...], "exit_code": N}}
    Tags are identified by [tag_name] lines, exit codes by [returns N] lines.

    Tag names may be either:
    - A plain preprocessor symbol: [FOO]
    - A valued define: [FOO=1] (will be passed as -DFOO=1)
    """
    test_file = Path(_primary_test_file(test_name))
    expect_file = CURRENT_DIR / f"{test_file.parent}/{test_file.stem}.expect"
    if not expect_file.exists():
        raise FileNotFoundError(f"Expect file not found: {expect_file}")

    tags = {}
    current_tag = None
    # Allow either [NAME] or [NAME=VALUE]. VALUE is captured verbatim (trimmed)
    # up to the closing bracket so it can express things like 1, 0x10, etc.
    tag_pattern = re.compile(r'^\[([a-zA-Z_][a-zA-Z0-9_]*)(?:=([^\]]+))?\]$')
    returns_pattern = re.compile(r'^\[returns (\d+)\]$')

    with open(expect_file, "r") as f:
        for line in f:
            stripped = line.rstrip('\n')

            # Check for tag marker
            tag_match = tag_pattern.match(stripped)
            if tag_match:
                name = tag_match.group(1)
                value = tag_match.group(2)
                if value is not None:
                    value = value.strip()
                    current_tag = f"{name}={value}"
                else:
                    current_tag = name
                tags[current_tag] = {"lines": [], "exit_code": 0}
                continue

            # Check for returns marker
            returns_match = returns_pattern.match(stripped)
            if returns_match and current_tag:
                tags[current_tag]["exit_code"] = int(returns_match.group(1))
                continue

            # Add line to current tag
            if current_tag and stripped:
                tags[current_tag]["lines"].append(stripped)

    return tags


def _sanitize_tag_for_filename(tag: str) -> str:
        """Make a tag safe to use in filenames/output suffixes.

        Examples:
            "test_var_2" -> "test_var_2"
            "TEST=1"     -> "TEST_1"
        """
        return re.sub(r"[^a-zA-Z0-9_]+", "_", tag).strip("_")


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


def _run_qemu_test(test_file, expected_exit_code, args=None, defines=None, opt_level="-O0", output_dir=None, timeout=10):
    expected_lines = load_expect_file(test_file)
    opt_suffix = f"_{opt_level.replace('-', '')}"
    config = CompileConfig(extra_cflags=opt_level, output_suffix=opt_suffix, output_dir=output_dir)
    sut, loglines = run_test(test_file, MACHINE, args, defines=defines, config=config)
    expected_lines = _strip_compiler_output(expected_lines, loglines)
    try:
        for line in expected_lines:
            _expect_line(sut, line, timeout=timeout)
        sut.wait()
        assert sut.exitstatus == expected_exit_code, f"Expected exit code {expected_exit_code}, got {sut.exitstatus}"
    except Exception as e:
        raise AssertionError(f"Test failed for {test_file} with {opt_level}: {e}") from e
    finally:
        sut.logfile.close()


def _run_tagged_qemu_test(test_file, tag, expected_lines, expected_exit_code, opt_level="-O0", output_dir=None):
    """Run a tagged test with specific define and expected output.

    Tagged tests may either:
    1. Fail to compile (expected compiler errors/warnings)
    2. Compile successfully and run with expected exit code and/or output
    3. Compile with warnings and run with expected output
    """
    test_files = [CURRENT_DIR / Path(test_file)]
    safe_tag = _sanitize_tag_for_filename(tag)
    opt_suffix = f"_{safe_tag}_{opt_level.replace('-', '')}"
    config = CompileConfig(defines=[tag], output_suffix=opt_suffix, extra_cflags=opt_level, output_dir=output_dir)
    test_name = Path(test_file).stem

    result = compile_testcase(test_files, MACHINE, config=config)

    # Write log file with compiler command and output
    log_path = str(result.elf_file.with_name(f"{result.elf_file.stem}_output.log"))
    with open(log_path, "w") as log_file:
        log_file.write(f"=== Compile: {test_file} {opt_level} with -D{tag} ===\n")
        if result.make_command:
            log_file.write(f"=== Make command: {' '.join(result.make_command)} ===\n")
        log_file.write(f"=== Compiler output ===\n")
        for line in result.output_lines:
            log_file.write(line + "\n")
        log_file.write(f"=== Success: {result.success} ===\n\n")

    compiler_output = "\n".join(result.output_lines)

    # Separate expected lines into compile-time and runtime
    # Compile-time lines typically contain the source filename
    source_basename = Path(test_file).name
    compile_expected = []
    runtime_expected = []
    for line in expected_lines:
        if line and source_basename in line:
            compile_expected.append(line)
        else:
            runtime_expected.append(line)

    # Verify compile-time expected lines in compiler output
    for line in compile_expected:
        if line and line not in compiler_output:
            raise AssertionError(
                f"Expected compile-time line not found for {test_file} [{tag}]:\n"
                f"Expected: {line}\n"
                f"Got:\n{compiler_output}"
            )

    # If compilation failed, we're done (compile error tests)
    if not result.success:
        return

    # Compilation succeeded - run the test
    sut = prepare_test(MACHINE, result.elf_file)
    log_file = open(log_path, "ab")  # Append runtime output
    log_file.write(b"=== Runtime output ===\n")
    sut.logfile = log_file

    try:
        # Match expected runtime output
        for line in runtime_expected:
            _expect_line(sut, line, timeout=1)
        sut.wait()
        assert sut.exitstatus == expected_exit_code, f"Expected exit code {expected_exit_code}, got {sut.exitstatus}"
    except Exception as e:
        raise AssertionError(f"Test failed for {test_file} [{tag}] with {opt_level}: {e}") from e
    finally:
        sut.logfile.close()


# Optimization levels to test
OPT_LEVELS = ["-O0", "-O1"]


def _generate_matrix_params(test_list):
    params = []
    ids = []
    for test_file, expected in test_list:
        # Support (exit_code,) or (exit_code, timeout) format
        if isinstance(expected, tuple):
            exit_code = expected[0]
            timeout = expected[1] if len(expected) > 1 else 10
        else:
            exit_code = expected
            timeout = 10
        for opt in OPT_LEVELS:
            params.append((test_file, exit_code, timeout, opt))
            ids.append(f"{_test_id(test_file)}{opt}")
    return params, ids


_MATRIX_PARAMS, _MATRIX_IDS = _generate_matrix_params(TEST_FILES)


@pytest.mark.parametrize("test_file,expected_exit_code,timeout,opt_level", _MATRIX_PARAMS, ids=_MATRIX_IDS)
def test_qemu_execution(test_file, expected_exit_code, timeout, opt_level, tmp_path):
    if test_file is None:
        pytest.fail("test_file is None")

    _run_qemu_test(test_file, expected_exit_code, opt_level=opt_level, output_dir=tmp_path, timeout=timeout)





def _generate_matrix_params_for_args(test_list_with_args):
    params = []
    ids = []
    for test_file, args, expected in test_list_with_args:
        for opt in OPT_LEVELS:
            params.append((test_file, args, expected, opt))
            ids.append(f"{_test_id(test_file)}{opt}")
    return params, ids


_MATRIX_ARGS_PARAMS, _MATRIX_ARGS_IDS = _generate_matrix_params_for_args(TEST_FILES_WITH_ARGS)


@pytest.mark.parametrize("test_file,args,expected_exit_code,opt_level", _MATRIX_ARGS_PARAMS, ids=_MATRIX_ARGS_IDS)
def test_qemu_execution_with_args(test_file, args, expected_exit_code, opt_level, tmp_path):
    if test_file is None:
        pytest.fail("test_file is None")

    _run_qemu_test(test_file, expected_exit_code, args=args, opt_level=opt_level, output_dir=tmp_path)


def _generate_tagged_test_params():
    """Generate test parameters for all tagged tests.

    Tags are auto-discovered from the .expect file.
    """
    params = []
    ids = []
    for test_file in TAGGED_TEST_FILES:
        tag_data = load_tagged_expect_file(test_file)
        for tag, data in tag_data.items():
            params.append((test_file, tag, data["lines"], data["exit_code"]))
            ids.append(f"{_test_id(test_file)}[{tag}]")
    return params, ids


_TAGGED_PARAMS, _TAGGED_IDS = _generate_tagged_test_params() if TAGGED_TEST_FILES else ([], [])


def _generate_tagged_matrix_params(tagged_params):
    params = []
    ids = []
    for test_file, tag, lines, exit_code in tagged_params:
        for opt in OPT_LEVELS:
            params.append((test_file, tag, lines, exit_code, opt))
            ids.append(f"{_test_id(test_file)}[{tag}]{opt}")
    return params, ids


_TAGGED_MATRIX_PARAMS, _TAGGED_MATRIX_IDS = _generate_tagged_matrix_params(_TAGGED_PARAMS) if _TAGGED_PARAMS else ([], [])


@pytest.mark.parametrize(
    "test_file,tag,expected_lines,expected_exit_code,opt_level",
    _TAGGED_MATRIX_PARAMS,
    ids=_TAGGED_MATRIX_IDS,
)
def test_qemu_tagged_execution(test_file, tag, expected_lines, expected_exit_code,opt_level, tmp_path):
    if test_file is None:
        pytest.fail("test_file is None")

    _run_tagged_qemu_test(test_file, tag, expected_lines, expected_exit_code, opt_level=opt_level, output_dir=tmp_path)



# TCC Compiler Bug Test Matrix
def _generate_tcc_bug_params():
    """Generate test parameters for TCC bug reproduction tests."""
    params = []
    ids = []
    for test_file, expected in TCC_BUG_TEST_FILES:
        for opt in OPT_LEVELS:
            params.append((test_file, expected, opt))
            ids.append(f"{_test_id(test_file)}{opt}")
    return params, ids


_TCC_BUG_PARAMS, _TCC_BUG_IDS = _generate_tcc_bug_params() if TCC_BUG_TEST_FILES else ([], [])


@pytest.mark.parametrize("test_file,expected_exit_code,opt_level", _TCC_BUG_PARAMS, ids=_TCC_BUG_IDS)
def test_tcc_compiler_bugs(test_file, expected_exit_code, opt_level, tmp_path):
    """Test cases for TCC compiler bug reproductions.

    These tests verify that previously fixed compiler bugs stay fixed:

    1. test_tcc_i64_ir_bug: "load_to_dest_ir I64/F64: dest.pr1 is spilled" error
       - Fixed: Handle case when pr1_spilled is set but pr1_reg is PREG_REG_NONE

    2. test_tcc_volatile_reg: Volatile memory-mapped register access issues
       - Fixed: Handle 64-bit constant load to 32-bit destination
    """
    if test_file is None:
        pytest.fail("test_file is None")

    _run_qemu_test(test_file, expected_exit_code, opt_level=opt_level, output_dir=tmp_path)
