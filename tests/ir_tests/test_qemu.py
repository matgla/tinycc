import pytest
import re
from pathlib import Path
from qemu_run import run_test, compile_testcase, CompileConfig, prepare_test, ASAN_ENABLED, VALGRIND_ENABLED


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
    ("bug_parse_number_64bit.c", 0),
    ("bug_ull_mul_int_accum.c", 0),
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
    ("105_builtin_strncmp_zero_count.c", 0),

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

    # SHA-1 regression tests (miscompiled under -O1)
    ("test_sha_transform.c", 0),
    ("test_mibench_sha.c", 0),

    # address-of on register-passed parameter (gaddrof() fix)
    ("bug_addrof_reg_param.c", 0),
    ("bug_addrof_param_modify.c", 0),

    # struct field post-increment in for-loop (spilled lvalue address fix)
    ("bug_struct_field_postinc.c", 0),

    # const char *const global pointer access (YAFF exported symbol section fix)
    ("bug_const_ptr_got_deref.c", 0),

    # union self-cast through typedef should not take the scalar-to-union extension path
    ("bug_union_self_cast_typedef.c", 0),

    # inline asm operands may reuse their own live registers in IR mode
    ("bug_inline_asm_reserved_regs.c", 0),

    # mul clobbers base register during struct array indexing (non-power-of-2 element size)
    ("bug_struct_array_index_mul_clobber.c", 0),

    # GNU attributes may prefix a declarator after a comma in a declaration list
    ("bug_decl_attr_after_comma.c", 0),

    # `__attribute__((alias(...)))` supports direct, asm-label, and forward targets
    ("bug_alias_attribute.c", 0),

    # comma expressions in sizeof must safely drop unused results without IR
    ("bug_sizeof_comma_func_decay.c", 0),

    # 64-bit left-shift in a loop clobbers adjacent pointer register/spill slot
    ("bug_ll_shift_ptr_clobber.c", 0),

    # for-loop increment lost when body has nested ternary chain as function arg
    ("bug_for_ternary_chain.c", 0),

    # identity comparison fold eliminates struct member comparisons with different addends
    ("bug_struct_member_cmp_fold.c", 0),

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

    # __builtin_classify_type tests
    ("140_builtin_classify_type.c", 0),

    # __builtin_bswap16, __builtin_bswap32, __builtin_bswap64 tests
    ("145_builtin_bswap.c", 0),

    # __builtin_add_overflow, __builtin_sub_overflow, __builtin_mul_overflow tests
    ("165_builtin_add_overflow.c", 0),

    # __builtin_add_overflow_p, __builtin_sub_overflow_p, __builtin_mul_overflow_p tests
    ("166_builtin_mul_overflow_p.c", 0),

    # IEEE 754 NaN comparison tests (soft-float GT/GE fix)
    ("170_nan_comparison.c", 0),

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
    ("test_switch_return.c", 0),  # Switch with direct return from each case (TBH backward targets)

    # sret hidden pointer consuming r0 must advance ABI call_layout.next_reg
    ("bug_sret_param_layout.c", 0),
    ("nested_basic.c", 0),
    ("nested_basic_args.c", 0),
    ("nested_multiple.c", 0),
    ("nested_capture_multiple.c", 0),
    ("nested_capture_array.c", 0),
    ("nested_capture_read.c", 0),
    ("nested_capture_write.c", 0),
    ("nested_direct_call_args.c", 0),
    ("nested_struct_return.c", 0),
    ("nested_shadowing.c", 0),
    ("nested_funcptr.c", 0),
    ("nested_funcptr_indirect.c", 0),
    ("nested_funcptr_call_twice.c", 0),
    ("nested_recursive_parent.c", 0),
    ("nested_multi_level.c", 0),

    # Complex number tests
    ("test_complex_fold.c", 0),
    ("test_complex_init.c", 0),
    ("test_complex_mul.c", 0),
    ("test_complex_simple.c", 0),

    ("111_builtin_printf.c", 0),
    ("112_builtin_puts.c", 0),
    ("150_builtin_fp.c", 0),
]

# Nested function tests expected to fail (not yet implemented)
NESTED_XFAIL_TEST_FILES = [
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

    # Bug: switch on char with sparse, non-contiguous case values (e.g. 'd','s','x')
    # TCC ARM codegen emits dispatch but omits the case bodies entirely,
    # jumping back to the loop top.  Breaks vfprintf format specifier handling.
    ("bug_switch_char_sparse.c", 0),

    # Bug: ternary inside while loop before sparse switch causes CODE_OFF_BIT
    # to remain set, making the switch handler skip dispatch generation entirely.
    ("bug_ternary_switch.c", 0),

    # Bug: Function pointer passed as 5th argument is clobbered before blx.
    # TCC loads the fn ptr into r1, then overwrites r1 with the 2nd call arg.
    # Reduced from musl libc qsort: fix(a, root, n, sz, cmp) where cmp is
    # called via blx with a data pointer instead of the comparator address.
    ("bug_funcptr_fifth_arg.c", 0),

    # Bug: Prologue scratch register clobbers incoming argument register.
    # When a parameter is spilled to stack at a large offset (>255, due to
    # char buf[1024]), tcc_gen_machine_store_to_stack uses another arg register
    # as scratch for the offset constant, destroying its value before it is saved.
    # Triggered by taking &fmt which forces it to a stack slot.
    ("bug_param_clobber_large_frame.c", 0),

    # Bug: switch with goto to common label loses large constant in OR.
    # case TOK_TYPEDEF: g = 0x4000; goto storage; storage: t |= g;
    # produces t=3 instead of t=0x4003 on native ARM compilation.
    # Reduced from parse_btype() where typedef/extern/static share a
    # "storage:" goto label. The constant 0x4000 (VT_TYPEDEF) is lost,
    # causing typedef declarations to fail with "invalid type for '__stack'".
    ("bug_switch_goto_or.c", 0),

    # Bug: gcase() single-value JUMPIF corrupts the default jump chain.
    # When a switch has >8 cases, gcase() builds a binary search tree.
    # The left subtree returns a JUMP linked to the default chain 'dsym'.
    # In the right subtree's linear scan, single-value cases passed dsym
    # to tcc_ir_codegen_test_gen(); tcc_ir_backpatch() then followed the
    # chain and rewrote the left subtree's fall-through to a case body.
    # Values not matching any case (e.g. tok='*'=42 in parse_btype) were
    # routed to a wrong handler instead of default, breaking typedef parsing.
    ("bug_switch_default_chain.c", 0),

    # Bug: post-increment fusion creates STORE_POSTINC instead of LOAD_POSTINC.
    # For ch = *p++, the optimizer fuses the STORE that writes back the
    # incremented pointer to its stack slot with the ADD, producing
    # str.w r1,[r4],#1 (store pointer to *p) instead of the LOAD from *p.
    # Corrupts input strings in parse_number, causing "invalid digit" errors.
    ("bug_postinc_store.c", 0),

    # Bug: Packed struct array stride computed incorrectly.
    # IR operand STACKOFF encoding stored offset/4 in aux_data, assuming
    # 4-byte alignment. Packed structs with non-power-of-2 sizes (e.g. 10)
    # produce non-aligned offsets (e.g. -30) whose lower 2 bits are lost.
    # Fix: store offset directly in aux_data without /4 compression.
    ("bug_stride_minimal.c", 0),
    ("bug_packed10_array.c", 0),
    ("bug_variant_stride.c", 0),
    ("bug_packed_sizes.c", 0),
    ("bug_stride10.c", 0),
    ("bug_bitfield_packed10.c", 0),
    ("bug_switch_bitfield.c", 0),


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
    opt_suffix = f"_{opt_level.replace('-', '').replace(' ', '_')}"
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


# Tests too slow under instrumentation (ASan / valgrind) — skip to avoid timeouts.
SLOW_UNDER_INSTRUMENTATION = {
    "../tests2/101_cleanup.c",
}


@pytest.mark.parametrize("test_file,expected_exit_code,timeout,opt_level", _MATRIX_PARAMS, ids=_MATRIX_IDS)
def test_qemu_execution(test_file, expected_exit_code, timeout, opt_level, tmp_path):
    if test_file is None:
        pytest.fail("test_file is None")
    primary = _primary_test_file(test_file) if isinstance(test_file, list) else test_file
    if (ASAN_ENABLED or VALGRIND_ENABLED) and primary in SLOW_UNDER_INSTRUMENTATION:
        pytest.skip("Skipped under ASan/valgrind (too slow)")

    _run_qemu_test(test_file, expected_exit_code, opt_level=opt_level, output_dir=tmp_path, timeout=timeout)


# Nested function xfail tests (not yet implemented)
def _generate_nested_xfail_params():
    params = []
    ids = []
    for test_file, expected in NESTED_XFAIL_TEST_FILES:
        for opt in OPT_LEVELS:
            params.append((test_file, expected, opt))
            ids.append(f"{_test_id(test_file)}{opt}")
    return params, ids


_NESTED_XFAIL_PARAMS, _NESTED_XFAIL_IDS = _generate_nested_xfail_params()


@pytest.mark.parametrize("test_file,expected_exit_code,opt_level", _NESTED_XFAIL_PARAMS, ids=_NESTED_XFAIL_IDS)
@pytest.mark.xfail(reason="Nested function feature not yet implemented")
def test_nested_xfail(test_file, expected_exit_code, opt_level, tmp_path):
    if test_file is None:
        pytest.fail("test_file is None")

    _run_qemu_test(test_file, expected_exit_code, opt_level=opt_level, output_dir=tmp_path)



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


# ---------------------------------------------------------------------------
# Tests requiring -ffunction-sections (separate .text.func sections)
# ---------------------------------------------------------------------------

FUNCTION_SECTIONS_TEST_FILES = [
    # Bug: Static function pointer resolved to wrong address under PIC with
    # text/data separation.  R_ARM_GOTOFF was used for static functions in
    # other text sections; GOTOFF only works within the same segment so the
    # address was garbage (jumped to _start instead of the callback).
    ("bug_static_func_reloc.c", 0),
]


def _generate_func_sections_params():
    params = []
    ids = []
    for test_file, expected in FUNCTION_SECTIONS_TEST_FILES:
        for opt in OPT_LEVELS:
            params.append((test_file, expected, opt))
            ids.append(f"{_test_id(test_file)}{opt}")
    return params, ids


_FUNC_SECTIONS_PARAMS, _FUNC_SECTIONS_IDS = _generate_func_sections_params() if FUNCTION_SECTIONS_TEST_FILES else ([], [])


@pytest.mark.parametrize("test_file,expected_exit_code,opt_level", _FUNC_SECTIONS_PARAMS, ids=_FUNC_SECTIONS_IDS)
def test_function_sections_bugs(test_file, expected_exit_code, opt_level, tmp_path):
    """Tests compiled with -ffunction-sections to trigger per-function text sections.

    This flag causes each function to be placed in a separate .text.funcname
    section, which exposes relocation bugs where static symbols in different
    text sections are treated incorrectly under PIC.
    """
    if test_file is None:
        pytest.fail("test_file is None")

    cflags = f"{opt_level} -ffunction-sections"
    _run_qemu_test(test_file, expected_exit_code, opt_level=cflags, output_dir=tmp_path)


# ---------------------------------------------------------------------------
# Tests requiring -fgnu89-inline
# ---------------------------------------------------------------------------

GNU89_INLINE_TEST_FILES = [
    # Regression: inline asm inside an `extern inline` function must still be
    # parsed, emitted, and callable when GNU89 inline semantics rewrite it to a
    # local out-of-line definition.
    ("bug_gnu89_inline_asm.c", 0),
]


def _generate_gnu89_inline_params():
    params = []
    ids = []
    for test_file, expected in GNU89_INLINE_TEST_FILES:
        for opt in OPT_LEVELS:
            params.append((test_file, expected, opt))
            ids.append(f"{_test_id(test_file)}{opt}")
    return params, ids


_GNU89_INLINE_PARAMS, _GNU89_INLINE_IDS = _generate_gnu89_inline_params() if GNU89_INLINE_TEST_FILES else ([], [])


@pytest.mark.parametrize("test_file,expected_exit_code,opt_level", _GNU89_INLINE_PARAMS, ids=_GNU89_INLINE_IDS)
def test_gnu89_inline_bugs(test_file, expected_exit_code, opt_level, tmp_path):
    """Tests compiled with -fgnu89-inline to exercise GNU89 extern-inline semantics."""
    if test_file is None:
        pytest.fail("test_file is None")

    cflags = f"{opt_level} -fgnu89-inline"
    _run_qemu_test(test_file, expected_exit_code, opt_level=cflags, output_dir=tmp_path)


# ---------------------------------------------------------------------------
# Tests requiring -mpic-data-is-text-relative (text/data separation PIC mode)
# ---------------------------------------------------------------------------

PIC_TEXT_DATA_SEP_TEST_FILES = [
    # Bug: const char *const global in .rodata was classified as YAFF_SECTION_CODE
    # in tcc_yaff_write_exported_symbols, so the dynamic loader resolved the
    # GOT entry to a garbage address (text_base + raw link-time VA).
    ("bug_const_ptr_got_deref.c", 0),

    # Bug: Register allocator picks wrong source register for local copy after
    # struct member load + AND mask under PIC text/data separation.  With R9
    # caller-saved (stmdb/ldmia around every call), register pressure causes
    # the copy of (call_site->registers_map & 0x0F) to pick the struct pointer
    # register instead of the AND result.  push_mask ends up with bit 13 (SP)
    # set → th_push returns {0,0}.
    ("bug_struct_mask_copy.c", 0),
]


def _generate_pic_text_data_sep_params():
    params = []
    ids = []
    for test_file, expected in PIC_TEXT_DATA_SEP_TEST_FILES:
        for opt in OPT_LEVELS:
            params.append((test_file, expected, opt))
            ids.append(f"{_test_id(test_file)}{opt}")
    return params, ids


_PIC_TDS_PARAMS, _PIC_TDS_IDS = _generate_pic_text_data_sep_params() if PIC_TEXT_DATA_SEP_TEST_FILES else ([], [])


@pytest.mark.parametrize("test_file,expected_exit_code,opt_level", _PIC_TDS_PARAMS, ids=_PIC_TDS_IDS)
def test_pic_text_data_separation(test_file, expected_exit_code, opt_level, tmp_path):
    """Tests compiled with -mpic-data-is-text-relative.

    This flag enables text/data separation mode where R9 holds the GOT base
    and code/data may be loaded at independent addresses.  Exposes bugs in
    YAFF symbol/relocation classification for .rodata globals.
    """
    if test_file is None:
        pytest.fail("test_file is None")

    cflags = f"{opt_level} -mpic-data-is-text-relative"
    _run_qemu_test(test_file, expected_exit_code, opt_level=cflags, output_dir=tmp_path)