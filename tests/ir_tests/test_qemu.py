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
    ("62_or_continue_shortcircuit.c", 0),
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
    ("bug_llong_min_const_cmp.c", 0),
    ("bug_ull_mul_int_accum.c", 0),
    ("bug_struct_slot_reuse.c", 0),
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
    ("106_string_ops_runtime.c", 0),

    # Single-precision float tests
    ("72_float_result.c", 1),  # Returns 1 on success (non-standard convention)
    ("73_float_ops.c", 1),     # Returns 1 on success

    # AEABI soft-float regressions (bit-level tests; avoids printf %f).
    ("test_aeabi_dmul_bits.c", 0),
    ("test_f2d_bits.c", 0),
    ("test_aeabi_double_all.c", 0),

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

    # SL-FWD multi-pred merge alias bug: inlined callee conditionally writes
    # through caller's stack ptr; caller post-call read must NOT forward the
    # pre-call value past the conditional store.  See SL_FWD_FIX_PLAN.md.
    ("test_sl_fwd_alias.c", 0),
    # Hand-crafted alias variants of the SL-FWD fix: must remain correct
    # without regressing forwarding for benign patterns.
    ("test_sl_fwd_alias_uncond.c", 0),
    ("test_sl_fwd_alias_call.c", 0),
    ("test_sl_fwd_alias_offsets.c", 0),

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
    ("421_fp_conformance.c", 0),
    ("422_const_pool_hoist.c", 0),

    # 64-bit bitfield extract: shl/shr->and fold, signed/word-crossing
    # non-fold cases, INT64 global deref CSE region invalidation
    ("423_llong_bitfield_extract.c", 0),

    # __builtin_isunordered lowered to a single __aeabi_[df]cmpun call
    ("424_isunordered_cmpun.c", 0),

    # GVN value-numbering of const runtime helpers (duplicate-call CSE)
    ("425_pure_call_cse.c", 0),

    # entry_store_prop must not forward a local's entry store through a pointer
    # that only may-alias it (phi of {param, &local}) or that was advanced
    ("426_entry_store_phi_alias.c", 0),

    # mem_inline must move a STRUCT slot's offset out of the split u.s encoding
    # when it narrows the slot's btype (offset was becoming offset << 16)
    ("427_mem_inline_struct_slot.c", 0),

    # Compile-time strlen constant folding
    ("171_strlen_constfold.c", 0),

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
    ("test_complex_real_mul.c", 0),
    ("test_complex_simple.c", 0),

    ("111_builtin_printf.c", 0),
    ("112_builtin_puts.c", 0),
    ("108_loop_unroll_basic.c", 0),
    ("109_loop_unroll_no_unroll.c", 0),
    ("110_loop_unroll_with_array.c", 0),
    ("113_reroll_basic.c", 0),
    ("114_reroll_negative.c", 0),
    ("150_builtin_fp.c", 0),

    # Benchmark regression tests (-O2 correctness)
    ("bench_fibonacci.c", 0),
    ("bench_bubble_sort.c", 0),
    ("bench_linked_list.c", 0),
    ("bench_binary_search.c", 0),
    ("bench_matrix_mul.c", 0),
    ("bench_function_calls.c", 0),
    ("bench_conditionals.c", 0),
    ("bench_switch_stmt.c", 0),
    ("bench_indirect_calls.c", 0),
    ("bench_array_sum.c", 0),
    ("bench_bitwise_mix.c", 0),
    ("bench_strcpy.c", 0),
    ("bench_memcpy.c", 0),
    ("bench_strcmp.c", 0),
    ("bench_strlen_scan.c", 0),

    # MiBench regression tests (-O2 correctness)
    ("mibench_bitcount.c", 0),
    ("mibench_crc32.c", 0),
    ("mibench_dijkstra.c", (0, 30)),  # Longer timeout for graph traversal
    ("mibench_qsort.c", 0),
    ("mibench_stringsearch.c", 0),
    ("mibench_sha.c", 0),
    ("mibench_rijndael.c", 0),
    ("172_const_agg_fold.c", 245),
    ("173_const_memcpy_fwd.c", 0),
    ("174_bitfield_extract_fold.c", 0),
    ("175_shift_pair_ubfx.c", 0),
    ("176_init_copy_global_fwd.c", 0),
    ("177_bfi_insert.c", 0),
    ("178_dead_store_sroa.c", 0),
    ("179_loop_carried_store.c", 0),
    ("180_loop_rotation_condbody.c", 0),
    ("181_loop_const_sim_extern_store.c", 0),
    ("182_init_copy_global_fwd_alu.c", 0),
    ("183_selfhost_inline_accumulate.c", 0),
    ("184_packed_bitfield_rmw_store.c", 0),
    # Loop unroll/rotation re-enable regression tests (wrong-code at -O1/-O2)
    ("185_loop_elim_zero_trip.c", 0),
    ("186_fuzz_nested_loop_rotation.c", 0),
    ("187_fuzz_loop_carried_scratch.c", 0),
    # Differential-fuzz O1/O2 miscompile regression tests (one per root cause)
    ("188_fuzz_dead_loop_split_backedge_phi.c", 0),
    ("189_fuzz_local_alu_cse_stackoff_var.c", 0),
    ("190_fuzz_mach_mod_src2_clobber.c", 0),
    ("191_fuzz_sccp_barrel_shift_fused.c", 0),
    ("192_fuzz_setif_litpool_highreg.c", 0),
    ("193_fuzz_entry_store_runtime_indexed.c", 0),
    ("194_fuzz_ssa_ternary_multidef_temp.c", 0),
    ("195_fuzz_ssa_ternary_multidef_temp2.c", 0),
    ("196_fuzz_mul_add_fuse_imm_dest.c", 0),
    ("197_fuzz_lea_fold_stack_alias.c", 0),
    ("198_fuzz_entry_store_ptr_overwrite.c", 0),
    ("199_fuzz_entry_store_forward_order.c", 0),
    ("200_fuzz_nonloop_phi_coalesce.c", 0),
    ("201_fuzz_xor_cancel_live_producer.c", 0),
    ("202_fuzz_cmp_stackoff_var_identity.c", 0),
    ("203_fuzz_unsigned_cmp_constprop.c", 0),
    ("204_fuzz_entry_store_loop_overwrite.c", 0),
    ("205_fuzz_jump_thread_dropped_store.c", 0),
    ("206_fuzz_disp_fusion_entry_store_indexed.c", 0),
    ("207_fuzz_literal_pool_branch_narrowing.c", 0),
    ("208_fuzz_var_tmp_fwd_intervening_store.c", 0),
    ("209_fuzz_sccp_degenerate_branch_unreachable.c", 0),
    ("210_fuzz_store_src_lea_hoist_intervening_store.c", 0),
    ("211_fuzz_load_cse_stack_indexed_runtime_store.c", 0),
    ("212_fuzz_cprop_copy_into_loop_phi.c", 0),
    ("213_fuzz_store_redundant_const_indexed_load.c", 0),
    ("214_fuzz_slfwd_unsigned32_i64_store_width.c", 0),
    ("215_fuzz_sccp_entry_init_indexed_store_clobber.c", 0),
    ("216_fuzz_loop_bound_remat_value_load.c", 0),
    ("217_fuzz_store_redundant_runtime_deref_alias.c", 0),
    ("218_fuzz_loop_unroll_branch_fallthrough.c", 0),
    ("219_fuzz_strd_spill_dryrun_offset.c", 0),
    ("220_fuzz_const_sim_branch_redef_liveness.c", 0),
    ("221_fuzz_inline_memcpy_param_named_local.c", 0),
    ("222_fuzz_strd_imm_spill_scratch_push_offset.c", 0),
    ("223_fuzz_loop_const_sim_fp_compare.c", 0),
    ("224_fuzz_const_branch_fold_skips_call.c", 0),
    ("225_fuzz_phi_simplify_barrel_shift_dangling_use.c", 0),
    ("226_fuzz_redundant_var_assign_addrof_alias.c", 0),
    ("227_fuzz_store_redundant_var_ptr_deref_read.c", 0),
    ("228_fuzz_entry_store_prop_var_ptr_alias.c", 0),
    ("229_fuzz_load_cse_var_addr_off0_alias.c", 0),
    ("230_fuzz_entry_store_var_runtime_array_ptr.c", 0),
    ("231_fuzz_loop_const_sim_bf_rmw_addrof_alias.c", 0),
    ("232_fuzz_bitfield_store_indexed_width.c", 0),
    ("233_fuzz_knownbits_subword_store_slot_overlap.c", 0),
    ("234_fuzz_switch_table_r12_clobber.c", 0),
    ("235_fuzz_retval_reg_share_store_ptr.c", 0),
    ("236_fuzz_post_ra_fwd_diamond_scratch_reassign.c", 0),
    # NOT a tcc bug: pins tcc's CORRECT output for a program the gcc oracle
    # miscompiles at -O2 (bitfield seed 1486); guards against a future regression.
    ("237_fuzz_bitfield_gcc_o2_miscompile.c", 0),
    ("238_fuzz_loop_const_sim_unsigned_char_residual.c", 0),
    ("239_fuzz_pack64_stack_slot_alias.c", 0),
    ("240_fuzz_block_copy_call_clobber.c", 0),
    ("241_fuzz_loop_const_sim_indexed_store.c", 0),
    ("242_fuzz_entry_store_runtime_base_indexed.c", 0),
    ("243_fuzz_value_track_uldivmod_stale_fwd.c", 0),
    ("244_fuzz_entry_store_rt_base_plus_imm.c", 0),
    ("245_fuzz_loop_const_sim_addr_plus_imm.c", 0),
    ("246_fuzz_loop_phi_coalesce_rotated_redef.c", 0),
    ("247_fuzz_gvn_64bit_truncating_copy.c", 0),
    ("248_fuzz_value_track_llsl_stale_fwd.c", 0),
    ("249_fuzz_loop_const_sim_else_arm_absorbed.c", 0),
    ("250_fuzz_var_const_fold_intervening_use.c", 0),
    ("251_fuzz_strd_pair_fuse_across_jump_target.c", 0),
    ("252_fuzz_knownbits_imm_subword_sext.c", 0),
    ("253_fuzz_ptr_load_cse_addrtaken_alias.c", 0),
    ("254_fuzz_it_block_literal_pool_flush.c", 0),
    ("255_fuzz_ssa_fold_64bit_shr_imm32.c", 0),
    ("256_fuzz_ptr_cprop_load_cse_pointee_def.c", 0),
    ("257_fuzz_ptr_mla_accum_dead_def.c", 0),
    # bug #2 re-enable: derived-IV strength reduction (va-arg-24 reduction +
    # register-only DIV positive case + single-trip CMP ptr,end soundness).
    ("258_derived_iv_strength_reduction.c", 0),
    # bug #7 sixth defect (ptr seeds 500/517): pure-call hoisting must not
    # treat an address-taken argument (mutated through pointers in-loop)
    # as loop-invariant.
    ("259_pure_call_hoist_addr_taken_arg.c", 0),
    # volatile seed 5053: MLA fusion sank a MUL's fused stack-slot read past
    # a loop store to the same slot by placing the MLA at the ADD's site.
    ("260_fuzz_mla_fusion_sinks_mem_read.c", 0),
    # ptr seed 7226: SSA use-list/count desync (load_cse fold left a stale
    # use record; DCE's count-only rebuild dropped a live deref use) made
    # DCE delete a pointer def that *p9 still dereferenced.
    ("261_fuzz_dce_use_list_count_desync.c", 0),
    # float seed 6632: dead_local_slot position-only liveness ignored loop
    # back-edges, killing a loop-carried store read at the loop top.
    ("262_fuzz_dead_local_slot_backedge.c", 0),
    # struct_byval seed 6105: real-run scratch PUSH in an FP-omitted frame
    # skewed SP-relative loads inside the push window by 4 bytes.
    ("263_fuzz_scratch_push_sp_offset.c", 0),
    # ptr seed 8507: ssa:load_cse's TVStore (store through an unresolved
    # TEMP pointer) survived a direct StackLoc store to the same address,
    # forwarding a stale constant into a later deref of that pointer.
    ("264_fuzz_load_cse_tvstore_stack_alias.c", 0),
    # switch seed 8261: float_branch's repeated zero-test fold NOP'd the
    # second `u8 & 1` test although u8 was redefined between the tests —
    # the spill-encoded STACKOFF reads compared structurally equal and the
    # plain-vreg XOR redefinition wasn't modeled as a memory mutation.
    ("265_fuzz_zero_test_refold_var_redef.c", 0),

    # volatile seed 8310: const_prop_tmp tracked a TEMP's folded constant but
    # never invalidated it on a non-constant redefinition of the same TEMP
    # position — loop unrolling's 16-temp rename cap leaves the 17th+ body
    # temp multi-def across unrolled copies, so iterations 1/2 read
    # iteration 0's stale constant.
    ("266_fuzz_const_prop_tmp_temp_redef.c", 0),

    # struct_byval seed 9494: value_tracking's generic source-read marking
    # only consumed src1/src2, so an MLA with a StackLoc src2 (no fold
    # pattern matched) never marked its accumulator VAR as read — a later
    # constant redef of the same VAR NOP'd the accumulator's def, leaving
    # `mla rd, rn, rm, ra` reading the caller's stale register.  Only
    # reproduces one call frame deep (main printf()s before the payload).
    ("267_fuzz_value_track_mla_accum_def.c", 0),

    # docs/bugs.md #7 (resolved), ninth defect; combo fuzz seeds
    # 52/80/187/311/333/392/460: pure-call hoisting's
    # insert_instruction_before patched JUMP/JUMPIF targets but not the
    # SWITCH_TABLE side table, leaving every case target stale by the
    # insertion count (infinite loops / wrong checksums / "missing
    # FUNCPARAMVAL" compile errors).
    ("268_pure_call_hoist_switch_table_targets.c", 0),

    # switch fuzz seed 10003 / ptr seed 19825 (O1/O2): redundant_var_assign
    # only saw src1/src2 reads, so a VAR read as an MLA accumulator looked
    # unread and its live defining load was NOP'd.
    ("269_fuzz_redundant_assign_mla_accum.c", 0),

    # struct_byval/combo fuzz seed 11651 (O1/O2): dse's write-only addr-TMP
    # scan and dead_lea_store's operand walk both missed the MLA accumulator
    # deref, deleting a by-value struct's spill stores that the MLA still read.
    ("270_fuzz_dse_mla_accum_deref.c", 0),

    # agg_deep fuzz seed 12085 (O1/O2): entry_store_prop's LEA map lost the
    # stack address at a TEMP<-TEMP ASSIGN copy, so a store through the copied
    # pointer never invalidated a BLOCK_COPY initializer and a stale constant
    # was forwarded.
    ("271_fuzz_entry_store_tmp_copy_alias.c", 0),

    # bitfield fuzz seed 12264 (O1/O2): sl_forward FORWARD-SUBBYTE/CROSS-MERGE
    # read stored_value.u.imm32 raw — for I64 pool immediates that's the pool
    # INDEX, so a packed-bitfield byte read forwarded garbage.
    ("272_fuzz_slfwd_subbyte_pool_imm.c", 0),

    # switch fuzz seed 18613 (O2): tcc_ir_build_cfg didn't mark SWITCH_TABLE
    # case/default targets as block leaders, so fall-through case entries
    # didn't split blocks and SCCP folded the checksum along the wrong case.
    ("273_fuzz_cfg_switch_target_leaders.c", 0),

    # bitfield fuzz seed 17717 (O1/O2): store_redundant's read scan missed the
    # MLA accumulator deref, killing a packed-struct field init store.
    ("274_fuzz_store_redundant_mla_accum.c", 0),

    # bitfield fuzz seeds 11840/11743/15654 (O2): loop_const_sim's memory map
    # had no width/overlap awareness — a packed-bitfield byte store left the
    # enclosing word slot's stale constant, and the collapsed RMW loop's
    # residual word store wiped the byte back to 0.
    ("275_fuzz_loop_const_sim_subword_overlap.c", 0),

    # switch fuzz seed 14009 (O2): sl_forward's post-forward store cleanup
    # missed live stores around runtime-indexed stack-array accesses.
    ("276_fuzz_entry_store_direct_index_loop.c", 0),

    # switch fuzz seed 17829 (O1): known_bits didn't mark SWITCH_TABLE
    # case/default targets as block starts, so a stack-slot fact from case 2
    # was reused on a direct jump to fall-through case 4.
    ("277_fuzz_known_bits_switch_target_merge.c", 0),

    # switch fuzz seed 18613 (O1/O2): full unroll grew case 0's counted loop
    # without shifting later SWITCH_TABLE case/default targets, so selector 3
    # entered the wrong point in the fall-through case chain.
    ("278_fuzz_unroll_switch_dispatch_loop.c", 0),

    # fp_round fuzz seed 18960 (O1): ssa:dce:phi_cycles removed loop-region
    # phis still needed by out-of-SSA phi resolution.
    ("279_fuzz_ssa_dce_phi_cycle_loop.c", 0),

    # volatile fuzz seed 16558 (O1/O2): ssa:var_to_param_forward substituted a
    # constant into a barrel-shift-annotated src2, silently dropping the LSL.
    ("280_fuzz_barrel_shift_var_fwd_imm.c", 0),

    # ptr fuzz seed 23598 (O1/O2): codegen MUL+ADD fusion bypassed the
    # consumer ADD's barrel-shift annotation, dropping a hidden LSR #18.
    ("281_fuzz_mul_add_fuse_barrel_annot.c", 0),

    # ptr fuzz seed 35289 (O1/O2): vrp compared sign-extended range endpoints
    # against a zero-extended pool-I64 CMP immediate, misfolding unsigned `<`.
    ("282_fuzz_vrp_unsigned_cmp_pool_imm.c", 0),

    # ptr fuzz seed 30436 (O1): scale-spec decodes bypassed the two-pass mop
    # cache; a dry-run allocation patch flipped the real-run's LOAD_INDEXED
    # coalesce decision, leaving a stale-cache copy that clobbered the load.
    ("283_fuzz_mop_cache_scale_desync.c", 0),

    # ptr fuzz seed 58108 (O1): SCCP's permissive entry-block store-forward
    # scan skipped a conditional *p store through a VAR-held pointer, folding
    # an array-element load back to its initializer.
    ("284_fuzz_sccp_entry_exempt_var_ptr_store.c", 0),

    # ptr fuzz seed 59549 (O2 HardFault): the MLA emitter didn't pre-exclude
    # deref operands' pointer registers; src2's spill reload clobbered the
    # deref-accumulator's pointer -> wild load (BFAR=0x8A4CB157).
    ("285_fuzz_mla_deref_accum_ptr_clobber.c", 0),

    # struct_byval/combo fuzz seed 26687 (O1/O2): dead_local_slot_elim's
    # tameness loop scanned only dest/src1/src2, never the MLA accumulator, so
    # a by-value struct field read through `MLA x*0 + Addr[StackLoc]***DEREF***`
    # let the field's home store be deleted (the STORE_INDEXED r.b write gated
    # off the mirrored precise-read path) -> MLA read an uninitialized slot.
    ("286_fuzz_mla_accum_deref_dead_slot.c", 0),

    # int fuzz seed 24769 (O1/O2/Os): guards the baseline integer stream case
    # from fuzz_triage_all_23000_31000.md.
    ("287_fuzz_int_24769.c", 0),

    # struct_byval/combo fuzz seed 34487 (O1/O2): ssa:load_cse did not
    # invalidate a tracked StackLoc store when a later PARAM store wrote the
    # same slot, so an sret field copy forwarded the stale initializer.
    ("288_fuzz_ssa_load_cse_param_store.c", 0),

    # varargs fuzz seed 31282 (O1/O2): const_var_prop exposed a variadic call
    # with stack-passed anonymous args to an ABI-sensitive backend miscompile.
    ("289_fuzz_varargs_const_var_prop_stack_call.c", 0),

    # varargs fuzz seed 36881 (O1/O2): barrel-shift fusion folded a const-prop'd
    # `x SHR #0` (identity) into a consuming OR as `orr ..., lsr #0`, which ARM
    # encodes as lsr #32 == 0; only LSL #0 is a true no-op barrel operand.
    ("290_fuzz_barrel_shift_zero_amount.c", 0),

    # agg_deep fuzz seed 36641 (O1/O2): redundant-store-elim killed a store to a
    # 2-D array slot that an intervening LOAD_INDEXED with a runtime base and a
    # constant column index could still read; the const-index branch never
    # flushed the array range for a runtime base.
    ("291_fuzz_rse_load_indexed_runtime_base.c", 0),

    # volatile fuzz seed 36818 (O2): post-RA move coalescing cleared a shared
    # register's live_regs_by_instruction bits when moving one of two
    # deliberately-overlapping claimants away; the phase-3 scratch-conflict
    # fixup then moved the outer loop counter onto the still-claimed register
    # and the inner loop's in-place XOR clobbered it (outer loop ran 1x not 4x).
    ("292_fuzz_move_coalesce_shared_reg_bitmap.c", 0),

    # bitfield fuzz seed 40979 (O1/O2): post-RA reverse move coalescing merged
    # a `u4 = u3` copy onto the source's register but only guarded against the
    # SRC being redefined while dest is live -- not the symmetric case where
    # DEST is redefined (`u4 = const`) while SRC (u3) is still read, clobbering
    # the shared register. Added a dest-redefinition guard to the reverse path.
    ("293_fuzz_move_coalesce_dest_redef.c", 0),

    # int fuzz seed 41379 (O1/O2): the narrow ADD/SUB CSE cse_param_add keyed a
    # stack local's lvalue read by a synthetic STACKOFF key, but a register-form
    # write to the same local (`u4 = <compare>`) only invalidated raw-vreg keys.
    # Two `u4 - #c` computations straddling the redefinition were wrongly CSE'd,
    # so the later one read the stale pre-assignment value. Fixed by having a
    # register-form write invalidate both the raw and STACKOFF synthetic key.
    ("294_fuzz_cse_param_add_stackoff_redef.c", 0),

    # signed fuzz seed 50156 (O1/O2): cmp_const_offset_fold proved `si7 = si6 -
    # 9033` from the outer-loop def and folded `si7 <= si6` to a constant, blind
    # to the inner back-edge redef `si7 = 659161088` that also reaches the CMP.
    # tcc_ir_find_defining_instruction is a linear scan; fixed by requiring both
    # CMP operands to be single-def before trusting the offset relationship.
    ("295_fuzz_cmp_offset_fold_backedge_redef.c", 0),

    # agg_deep fuzz seeds 52367/53515 (O2 HardFault): the codegen ASSIGN-lowering
    # STRD peephole fused a `T <- *ppa` def (an ASSIGN whose REG src has
    # needs_deref) into a plain reg->spill STRD, spilling the pointer raw and
    # dropping a level of indirection; the later `*T = x` corrupted the pointer
    # and the next `**ppa` read faulted. Fixed by mirroring the !src1.needs_deref
    # guard the STORE/STORE_INDEXED STRD peepholes already use.
    ("296_fuzz_assign_strd_deref_src.c", 0),

    # ptr fuzz seed 72674 (O2): sl_forward re-validated a multiply-defined merge
    # temp (a ?: diamond result) after forwarding its else-arm LOAD, so the merge
    # store resolved through the stale else-arm value and the following load
    # forwarded the wrong arm. Fixed by rejecting multi-def temps when consuming
    # the fwd_tmp_val tracking table.
    ("297_fuzz_slfwd_multidef_merge_temp.c", 0),

    # struct_byval fuzz seed 60351 (O2): same sl_forward multiply-defined ?:
    # merge-temp root cause as seed 72674, reached from the struct-by-value
    # profile -- the ternary result is stored into a by-value struct argument
    # slot before being read back, so the wrong ?: arm was forwarded through the
    # struct store. Fixed by the same fwd_tmp_defs < 2 consume-site guards.
    ("298_fuzz_slfwd_struct_merge.c", 0),

    # volatile fuzz seed 64026 (O1 internal compiler error): the identical-block
    # loop re-roller (ir/opt_reroll.c) matched a phase-shifted window over a run
    # of `PARAM0; PARAM1; CALL` call groups, placing the period boundary between
    # a call's params and its own CALL. Re-rolling the shifted window NOP'd the
    # last call's FUNCPARAMVAL markers while leaving its FUNCCALLVAL standing, so
    # the backend callsite scan aborted with "missing FUNCPARAMVAL for call_id=N".
    # Fixed by requiring the canonical body to be call-balanced so the boundary
    # lands on a real call-group edge (natural alignment).
    ("299_fuzz_reroll_call_phase_split.c", 0),

    # combo fuzz seed 74935 (O2): SSA copy-propagation (ssa_opt_cprop's
    # ssa_gen_cprop_copy_var_stackoff) forwarded an address-taken local
    # `u10` across an aliasing store `*p11 = k` (p11 == &u10) into the uses
    # of a `u9 = u10 ^ 0` copy temp. The barrier scan only bailed on a direct
    # redef of u10's vreg, missing the deref store (whose dest is the pointer,
    # not u10), so the forwarded read saw the clobbered slot. Fixed by bailing
    # on any intervening memory store when the STACKOFF source is address-taken;
    # the sibling ssa_gen_cprop_copy_param got the same guard.
    ("300_fuzz_cprop_var_stackoff_alias_store.c", 0),

    # combo_num seed 84127 (O1) / ptr seed 80958 (O2): the 64-bit register-pair
    # call-crossing eviction fallback in ra_linear_scan spilled a
    # loop_phi_locked single-INT victim (a loop counter sharing its register
    # with a live coalesce partner) to free a pair, double-booking the register
    # with a 64-bit value's high half -> clobbered loop counter.  Fixed by
    # skipping loop_phi_locked victims, as the single-register spill path does.
    ("301_fuzz_llong_pair_evict_loop_phi.c", 0),

    # agg_deep seed 86393 (O1): tcc_ir_opt_ptr_load_cse forwarded a pointer
    # deref (`**ppa212` == u4's slot; u4 is address-taken) across an aliasing
    # store to u4.  The pass flushed its deref cache on a register-form write to
    # an address-taken VAR but not on the is_lval ASSIGN form the frontend emits
    # when the VAR is materialized to memory (it is read via a pointer after),
    # and that ASSIGN is not a STORE op, so the cache was never invalidated and
    # the second `(**ppa212) & 31` re-used the stale pre-store value.  Fixed by
    # flushing on ANY write to an address-taken VAR; the sibling local ALU-CSE
    # pass got the same treatment for cached deref (lval-src) entries.
    ("302_fuzz_ptr_load_cse_addrtaken_lval_store.c", 0),

    # ptr seed 80958 (O2): ptr_store_load_fwd (Phase 6b) NOP'd a live store as
    # redundant because an intervening runtime-index LOAD_INDEXED that reads it
    # was not registered as a read.  Two stores to arr[1] straddle an
    # `arr[i&7]` read; after const_prop_tmp folded both offsets to `+4`,
    # local_alu_cse coalesced their addresses to one vreg, so the second store
    # killed the first — but when i&7==1 the load reads arr[1], feeding the
    # second store, so the first is live.  Fixed by marking pending stores as
    # loaded on any LOAD_INDEXED (RSE runtime-base class, agg_deep seed 36641).
    ("303_fuzz_pslfwd_indexed_read_alias.c", 0),

    # ptr seed 85636 (O1): add_reassoc forwarded an address-taken local's
    # arithmetic def (`u4 = u3 + C1`) across an aliasing pointer store `*p7=...`
    # (p7==&u4) that redefined u4, then folded `u4 + C2` into `u3 + (C1+C2)`
    # off u4's stale pre-store value.  The linear def lookup is blind to the
    # store; fixed by bailing when the forwarded base or its inner var is an
    # address-taken VAR and a memory-clobbering STORE/CALL sits in the gap.
    ("304_fuzz_add_reassoc_addrtaken_alias.c", 0),

    # longlong seed 111125 (vs-gcc): opt_bitfield's masked-extract fold used
    # tcc_ir_find_defining_instruction on a TEMP with multiple reaching defs,
    # saw only a zero-valued arm, and folded `(T | C) & 1` to `T`.
    ("306_fuzz_bitfield_multidef_masked_extract.c", 0),
    ("307_fuzz_bitfield_slfwd_packed_rmw.c", 0),
    ("308_fuzz_unroll_indexed_store_alias.c", 0),
    ("309_fuzz_lea_rse_struct_byval.c", 0),
    ("310_fuzz_dse_longlong_loop_store.c", 0),
    ("311_fuzz_mla_fusion_agg_alias.c", 0),
    ("312_fuzz_slfwd_indexed_agg_store.c", 0),
    ("313_fuzz_varargs_slforward_va_list.c", 0),
    ("314_fuzz_varargs_slforward_call_site.c", 0),
    ("315_fuzz_var_to_param_fwd_store_indexed_width.c", 0),
    ("316_fuzz_ptr_load_cse_var_redef.c", 0),
    ("317_fuzz_loop_elim_missing_exit_jump.c", 0),
    ("318_fuzz_dead_loop_elim_missing_exit_jump.c", 0),

    # SSA optimizer regression tests (ir/opt/ssa_opt*.c)
    ("319_ssa_branch_unsigned_cmp.c", 0),
    ("320_ssa_branch_reflexive.c", 0),
    ("321_ssa_cmp_eq_dom_facts.c", 0),
    ("322_ssa_cprop_copy_chain.c", 0),
    ("323_ssa_cprop_var_forward.c", 0),
    ("324_ssa_cprop_var_const_fold_intervening.c", 0),
    ("325_ssa_dce_dead_phi_cycle.c", 0),
    ("326_ssa_dce_unreachable.c", 0),
    ("327_ssa_dead_loop_const_bound.c", 0),
    ("328_ssa_dead_loop_runtime_bound.c", 0),
    ("329_ssa_fold_identities.c", 0),
    ("330_ssa_fold_64bit_const.c", 0),
    ("331_ssa_opt_phi_two_phis_same_incoming.c", 0),
    ("332_ssa_opt_addrtaken_locals.c", 0),

    # Fuzz regression tests: MUL/DIV/MOD src2-deref clobber (switch 219754,
    # ptr 291660), sl_forward LEA-map deref-value offset (ptr 260222),
    # ra_build_assign_hints deref-load coalesce (varargs 293237), and gen_opif
    # double-precision constant-fold double-rounding (float 206597/268558).
    ("333_fuzz_mul_deref_src2_clobber.c", 0),
    ("334_fuzz_mul_deref_ptr_profile.c", 0),
    ("335_fuzz_slfwd_lea_deref_offset.c", 0),
    ("336_fuzz_varargs_ra_hint_deref.c", 0),
    ("337_fuzz_genopif_double_round.c", 0),
    ("338_fuzz_genopif_double_round2.c", 0),
    # combined-sweep seed 320164 (O2): sl_forward's STORE->STORE forward recorded
    # a VAR-vreg-dest store for dead-store elim without the anonymous-slot guard;
    # the post-pass still-read scan (is_local operands only) missed the VAR's
    # direct-vreg arithmetic readers and deleted a live store.  Diverged in every
    # generator profile at this seed -- a single root cause.
    ("339_fuzz_slfwd_dse_var_store_vreg_read.c", 0),

    # ptr fuzz seed 380495 (O1/O2 wrong): ssa:load_cse skipped iload invalidation
    # for STACKOFF-dest stores, but the canonical TEMP-DEREF LOAD CSE now tracks
    # VAR-pointer bases that point into the local frame (p5 = &arr4[i]).  A direct
    # stack store `arr4[1]=...` (== *p5) failed to kill the cached *p5 load.
    ("340_fuzz_load_cse_stack_store_var_ptr.c", 0),

    # ptr fuzz seed 409667 (O1/O2 wrong): add_reassoc folded `x = u4 + C2` into
    # `x = base + (C1+C2)` via `u4 = base + C1` where base == StackLoc[-4] (arr6[7]),
    # a raw stack slot with no backing vreg.  The seed-85636 aliasing guard only
    # rejected address-taken VAR bases, so an intervening `*p7 = ...` store
    # (p7 == &arr6[7]) that clobbered the slot slipped through and the fold reused
    # the post-store value.  Now bails on a direct memory-slot base (is_lval,
    # inner_vr < 0) across a gap memory clobber.
    ("341_fuzz_add_reassoc_stack_slot_alias.c", 0),

    # switch fuzz seed 457962 (O2 wrong): linear-scan RA expire loop returned a
    # hard register to the free pool when the shorter of two coalesced intervals
    # sharing it expired, while the longer merge-temp interval (u6, live across a
    # trailing loop) still held it.  A loop-body temp (u5 = st9.f2 | 146) then
    # reused R6, so the post-loop `csmix(cs, u6)` read u5 instead of u6.  Fixed by
    # never freeing a register a surviving active interval still occupies.
    ("342_fuzz_ra_expire_coalesced_reg_share.c", 0),

    # Promoted from orphan triage: builtins, _Complex, aggregate init,
    # 64-bit ops, cast/bitfield, and previously-fixed bug regressions.
    # Verified against the gcc -m32 -funsigned-char oracle.
    ("141_builtin_signbit.c", 0),
    ("142_builtin_copysign.c", 0),
    ("150_builtin_setjmp.c", 0),
    ("160_builtin_prefetch.c", 0),
    ("95_ternary_array.c", 0),
    ("96_compound_array_init.c", 0),
    ("99_struct_init_inline.c", 0),
    ("99_struct_init_narrow.c", 0),
    ("50_complex_types.c", 0),
    ("51_complex_arith.c", 0),
    ("21_char_array.c", 0),
    ("test_cast_bitfield.c", 0),
    ("test_cast_bitfield2.c", 0),
    ("test_llong_shr.c", 0),
    ("test_u64_cmp.c", 0),
    ("test_u64_shift.c", 0),
    ("test_return64.c", 0),
    ("test_fp_cache_callee_saved.c", 0),
    ("ehabi_unwind_test.c", 0),
    ("matrix_test_simple.c", 0),
    ("nested_basic_simple.c", 0),
    ("bug_global_field_short_circuit.c", 0),
    ("bug_index_increment.c", 0),
    ("bug_irop_packed_9byte.c", 0),
    ("bug_local_var_printf_o1.c", 0),
    ("bug_macro_local_o1.c", 0),
    ("bug_postinc_struct.c", 0),
    ("bug_sl_fwd_wrong_addr.c", 0),
    ("bug_switch_in_loop.c", 0),
    ("bug_union_field_read.c", 0),

    # C11 _Pragma operator: pack layout via literal + DO_PRAGMA macro idiom.
    ("343_pragma_operator.c", 5),

    # First-iteration-exit loop elimination (20070824-1.c pointer-chase shape
    # + runtime control loops); pins behavior across the legacy ->
    # ssa:first_iter_exit migration.
    ("344_first_iter_exit.c", 0),

    # Pointer-IV exit-value substitution (pr49644 idiom + runtime-trip
    # control); pins behavior across the legacy -> ssa:ptr_iv_exit_subst
    # migration.
    ("345_ptr_iv_exit_subst.c", 0),

    # Loop constant simulation (soft-float accumulator + residual-fed cascade +
    # runtime-bounded control loop); pins behavior across the legacy Phase 4e ->
    # ssa:loop_const_sim migration.
    ("346_loop_const_sim_ssa.c", 0),

    # Loop unrolling / constant-trip elimination (accumulator + symbolic-limit
    # SELECT with zero-trip guard + then-arm need_exit_jump + runtime control);
    # pins behavior across the legacy Phase 5a -> ssa:loop_unroll migration.
    ("347_loop_unroll_ssa.c", 0),

    # Dead-loop elimination: the "distinctive legacy domains" (address-taken
    # memory-VAR + self-store) as correctness pins across the retirement of the
    # legacy tcc_ir_opt_dead_loop_elim (ssa:dead_loop now owns collapse).  The
    # memvar_rt(0) case pins that we do NOT do the legacy's unsound preheader
    # hoist.  See docs/plan_legacy_loop_dead_loop_elim_ssa.md.
    ("348_dead_loop_elim_retired_shapes.c", 0),
    # Decrement-to-zero: count-up -> count-down-to-zero rewrite
    # (ssa:decrement_to_zero) + the codegen SUBS/CMP#0 fusion that consumes it.
    # Correctness must hold at every -O level; pure-counter purestore loops get
    # the count-down latch at -O1+, IV-read / runtime-limit shapes decline.
    # See docs/plan_legacy_loop_decrement_to_zero_ssa.md.
    ("349_decrement_to_zero.c", 0),
    # ssa:reroll — identical-block re-rolling relocated to the post-propagation
    # regalloc flat region.  Pins (A) the fuzz-sensitive call-rerolling path
    # (period-3 opaque calls re-roll into a counted loop, calls/accumulation
    # exact) and (B) that foldable macro-unrolled runs still collapse downstream
    # (the win over the legacy pre-propagation placement).
    # See docs/plan_legacy_loop_reroll_ssa.md.
    ("350_reroll_ssa.c", 0),
    # stack_addr_nonnull_fold must not fold the loop-exit compare of a WALKING
    # stack pointer (base != p while p decrements to reach base) as "distinct
    # addresses never equal" — that drops the loop exit and collapses the caller
    # under DCE.  Reduced from gcc.c-torture 990513-1, unmasked by relocating
    # reroll to ssa:reroll (post-propagation).  Fixed via the in_loop guard.
    # See docs/plan_legacy_loop_reroll_ssa.md.
    ("351_walk_ptr_cmp_nonnull_fold.c", 0),
    ("352_ssa_const_string_fold.c", 0),
    ("353_ssa_symref_addend_fold.c", 0),
    ("354_ssa_global_addr_hoist.c", 0),
    ("355_ssa_sbfx_const_fold.c", 0),
    ("356_ssa_bitop_const_fold.c", 0),
    ("357_ssa_string_search_fold.c", 0),
    ("358_ssa_clrsb_fold.c", 0),
    ("359_signed_div_pow2.c", 0),
    ("360_self_arith_fold.c", 0),
    ("361_cmp_offset_common_base.c", 0),
    ("362_value_track_const_lmod_fold.c", 0),
    ("363_pure_modulo_hoist_cse.c", 0),
    ("364_diamond_store_fwd.c", 0),
    ("365_stack_addr_distinct_locals_cmp.c", 0),
    ("366_ssa_memchr_fold.c", 0),
    ("367_ssa_addr_cse_hoist.c", 0),
    ("368_native_bit_builtins.c", 0),
    ("369_switch_operand_reuse.c", 0),
    ("370_ptr_struct_copy_inline.c", 0),
    ("371_forward_branch_narrowing.c", 0),
    ("372_ldrd_align_deref.c", 0),
    ("373_loop_rotate_global_call.c", 0),
    ("380_coalesce_diamond_pair.c", 0),
    ("381_fuzz_cmp_fold_pooled_imm_width.c", 0),
    ("382_fuzz_guard_collapse_phi_cfg_desync.c", 0),
    ("383_fuzz_softfp_cmp_fold_phi_prune.c", 0),
    ("384_fuzz_load_cse_barrel_shift_imm.c", 0),
    ("385_fuzz_dead_loop_imm_store_width.c", 0),
    ("386_fuzz_cmp_fold_barrel_shift_src2.c", 0),
    ("387_fuzz_licm_pair_call_hoist.c", 0),
    ("388_fuzz_licm_partial_chain_hoist.c", 0),
    ("389_fuzz_sccp_split_loop_range_clobber.c", 0),
    ("390_fuzz_dead_loop_double_phi_imm.c", 0),
    ("391_fuzz_cbz_pool_flush_cushion.c", 0),
    ("392_fuzz_sccp_phi_imm_store_width.c", 0),
    ("393_fuzz_flat_cmp_fold_barrel_annot.c", 0),
    ("394_fuzz_barrel_shift_imm_remat_drop.c", 0),
    ("395_fuzz_setif_xor_invert_diamond_join.c", 0),
    ("396_fuzz_fold_double_neg_barrel_shift.c", 0),
    ("397_fuzz_load_cse_indexed_frame_alias.c", 0),
    ("398_fuzz_sccp_phi_mla_accum.c", 0),
    ("399_fuzz_sccp_phi_mla_accum_loop.c", 0),

    # SSA: a full-width slot STORE to an upward-exposed local is a fresh
    # definition that phi placement must see (ir/ssa.c
    # ssa_store_slot_def_pos).  Pins the join / loop-carried / aliased
    # shapes the STORE-def path can get wrong.
    ("400_ssa_store_def_global_var.c", 0),

    # Codegen: SHL->ADD barrel fusion (and the scaled-addressing shapes it must
    # leave alone), plus the SETIF `& mask` identity that unblocks
    # setif_branch_fuse.
    ("401_barrel_shl_add_setif_mask.c", 0),

    # Loops: zero-trip entry-guard elimination across a chain of sequential
    # counted loops (source/opt/flat/loop/seq_guard_elim.c) and the rotation
    # gate that depends on it.  Pins the exit-value carry, the shapes the
    # walker must decline (zero-trip middle guard, runtime entry, address-taken
    # IV, a branch between the loops, negative step).
    ("402_seq_loop_guard_elim.c", 0),

    # SSA narrowing: mask-composition fold in narrow_and.  A bitfield read
    # narrower than its storage unit emits truncate-to-container then
    # mask-to-field; the inner mask is backward-redundant and the two compose
    # (a UBFX(v,0,w) counts as an AND), emitting the canonical UBFX form.
    ("403_mask_compose_narrow.c", 0),
    ("404_licm_escaped_dest.c", 0),
    ("405_cmp_const_reverse.c", 0),
    # Element-major fusion of chained vector_size expressions: a consumer
    # recomputes its operands' elements inline and deletes the producer's loop.
    ("406_vector_elem_major_fusion.c", 0),
    # `(x^y) cmp y` -> `x cmp 0`, which holds for == / != but not for the
    # relational predicates (N/C/V are not preserved).
    ("407_cmp_xor_cancel.c", 0),
    # copy_source_load_fwd: redirecting `x.f` to `G.f` after `x = G` must respect
    # per-field disjointness and the demanded bits of a bitfield read — a write
    # to a different field (or to other bits of the same word) may not block the
    # forward, but a write to the read's own bits must.
    ("408_copy_source_bitfield_fwd.c", 0),
    # SETIF state-mask algebra + 0/1 re-normalization elimination (PR107881):
    # two booleans over the same operand pair collapse to one compare, and a
    # value already known to be 0/1 needs no `!= 0`.  Signedness must not merge
    # across the mask, non-boolean values must keep the real compare, and
    # widening a SETIF into a binary op needs a fresh operand-pool block.
    ("409_setif_mask_bool_norm.c", 0),
    # Pruned SSA phi placement: a phi goes only where the VAR is live-in, not
    # over the whole iterated dominance frontier of its defs.  Block-scoped
    # loop-body vars lose their dead loop-header/latch phi pair (and the
    # slot-to-slot copy SSA destruction made of it), while accumulators,
    # branch-arm defs, in-place 64-bit writes, dereferenced pointer vars and
    # loop-live-out values must all keep theirs.
    ("410_pruned_ssa_live_in.c", 0),
    ("411_entry_store_var_ptr_base.c", 0),
    ("412_subword_store_merge.c", 0),
    ("413_scratch_demote_spill.c", 0),
    ("414_fuzz_cprop_assign_store_redef.c", 0),
    ("415_fuzz_slfwd_load_into_var.c", 0),
    ("416_fuzz_cprop_phi_store_redef.c", 0),
    ("417_fuzz_const_prop_narrow_dest.c", 0),
    ("418_fuzz_slfwd_call_dest_var.c", 0),
    ("419_loop_const_sim_wide_trip.c", 0),
    ("420_bitfield_unit_narrow.c", 0),
]

# Per-test compiler defines (e.g. for missing platform macros)
# Maps test filename -> list of defines passed as -D flags
TEST_FILE_DEFINES = {
    "mibench_sha.c": ["LITTLE_ENDIAN"],  # newlib doesn't provide this unlike glibc
}

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

    # Bug: GNU ?: (Elvis operator) extension miscompiled - picks wrong branch.
    # `tt ?: fallback` always evaluates to fallback even when tt is non-null.
    # Caused toybox cp to use source filename as destination, triggering
    # "same file" error.  Workaround: expand to explicit `tt ? tt : fallback`.
    ("bug_gnu_ternary_elvis.c", 0),

    # Self-host codegen bugs found compiling tinycc for YasOS (build_rootfs.sh).
    # Bug: dead-loop elimination reused a NOP slot's stale operand_base when
    # widening it to ASSIGN, overflowing into the next instruction's dest and
    # corrupting it into an immediate -> "mach_get_dest_reg: unexpected kind 3".
    ("bug_dead_loop_assign_overlap.c", 0),
    # Bug: ssa_opt_cmp_eq_prop pushed an equality fact from a loop back-edge into
    # the loop header's dominator subtree when the header is also the function
    # entry (its only CFG predecessor is the back-edge), folding the in-loop
    # `if (c1 != c2) return ...;` to "always equal".  Broke strncasecmp at -O1,
    # which made toybox `ps` print help instead of the process table.
    ("bug_cmp_eq_loop_header_entry.c", 0),
    # Bug: a switch-of-constants rewritten to SWITCH_LOAD spilled its dest under
    # register pressure -> "SWITCH_LOAD dest must be in a hardware register".
    ("bug_switch_load_spill.c", 0),
    # Bug: mla-fusion formed a 64-bit MLA for a non-in-place accumulate (dest !=
    # accumulator), which SMLAL/UMLAL cannot lower -> "unable to lower 64-bit MLA".
    ("bug_mla64_non_inplace.c", 0),
    # Bug: SSA rename cleared is_lval on a deref store/load through a promoted
    # pointer var -> `(v=call())->m0=c` (member offset 0) lowered `*v=c` to `v=c`,
    # dropping the store and clobbering the pointer (HardFault in toybox sh).
    ("bug_chained_assign_store_off0.c", 0),
    # Bug: loading a stack-passed parameter into an "unresolved" transient
    # (PREG_NONE, frame offset 0) lowered an offset-0 spill as `str rX,[FP,#0]`,
    # clobbering the saved frame record (r7) under the FP prologue -> caller's
    # frame pointer corrupted on return (HardFault/STKOF in tinycc new_symtab).
    ("bug_param_spill_fp_off0.c", 0),
    # Bug: the CBZ/CBNZ peephole committed a 2-byte forward branch from a wrong
    # distance estimate -> "CBZ/CBNZ target out of range" when the body > 126 bytes.
    ("bug_cbz_far_zero_branch.c", 0),
    # Bug: IV strength-reduction mis-shifted instructions for derived-IV address
    # expressions feeding a struct-copy call, deleting a PARAM and crashing with
    # "missing FUNCPARAMVAL for call_id=N" (in-place struct-array compaction).
    ("bug_ivsr_struct_compact.c", 0),
    # Bug: the post-increment lowering (LOAD_POSTINC/STORE_POSTINC) wrote back
    # only the loaded/stored value, not the post-incremented pointer.  A SPILLED
    # loop-carried pointer never advanced (the `ldrb [rN],#1` bumped only a
    # scratch reg) -> `*q++` re-read the same byte forever.  tcc hung in
    # parse_number() compiling ANY integer literal (self-hosted compiler froze).
    ("bug_postinc_spilled_ptr.c", 0),
    # Bug: CMP identity-folding ignored operand lval-ness, folding the
    # `ptr >= array + N` bounds check (where the pointer field aliases
    # &array[N]) to always-true and dropping the guard.  This is tcc's own
    # ifdef_stack overflow check -> the first `#if` in the predefs reported
    # "memory full (ifdef)" and the self-hosted compiler couldn't preprocess.
    ("bug_cmp_ptr_array_alias.c", 0),


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
    """Load and return lines from .expect file and expected exit code.

    Recognises [returns N] directives: the last one found sets the
    expected exit code (returned as second element).  Those lines are
    excluded from the expected-output list.
    """
    test_file = Path(_primary_test_file(test_name))
    expect_file = CURRENT_DIR / f"{test_file.parent}/{test_file.stem}.expect"
    if not expect_file.exists():
        raise FileNotFoundError(f"Expect file not found: {expect_file}")

    lines = []
    exit_code = None
    returns_pattern = re.compile(r'^\[returns (\d+)\]$')

    with open(expect_file, "r") as f:
        for line in f:
            stripped = line.rstrip('\n')
            m = returns_pattern.match(stripped)
            if m:
                exit_code = int(m.group(1))
            else:
                lines.append(stripped)

    return lines, exit_code


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


def _run_qemu_test(test_file, expected_exit_code, args=None, defines=None, opt_level="-O0", output_dir=None, timeout=10,
                   float_abi=None):
    expected_lines, expect_exit = load_expect_file(test_file)
    if expect_exit is not None:
        expected_exit_code = expect_exit
    opt_suffix = f"_{opt_level.replace('-', '').replace(' ', '_')}"
    if float_abi:
        opt_suffix += f"_{float_abi}"
    config = CompileConfig(extra_cflags=opt_level, output_suffix=opt_suffix, output_dir=output_dir,
                           float_abi=float_abi)
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
        sut.close()
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
        sut.close()
        sut.logfile.close()


# Optimization levels to test
# -Os is the level toybox/yasos apps build at; several miscompiles (e.g. the
# value-tracking store-through-pointer-var bug) only surface under -Os, so it
# must be in the matrix even though -O0/-O1/-O2 pass.
OPT_LEVELS = ["-O0", "-O1", "-O2", "-Os"]


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

    defines = TEST_FILE_DEFINES.get(primary)
    _run_qemu_test(test_file, expected_exit_code, defines=defines, opt_level=opt_level, output_dir=tmp_path, timeout=timeout)


# Hard-float (-mfloat-abi=hard) execution gate.  Compiles with the VFP
# single-precision register class (MACH_OP_VFP_REG) across all opt levels and
# checks the numeric result via exit code.  fp_hard_mixed_exec.c covers mixed
# int+float parameter passing, which used to miscompile (the VFP≡GPR aliasing —
# see docs/plan_vfp_hard_float.md) and is now correct.
HARD_FLOAT_TEST_FILES = [
    ("fp_hard_sp_exec.c", 1),
    ("fp_hard_mixed_exec.c", 1),
    ("fp_hard_mem_exec.c", 1),
    ("fp_hard_call_exec.c", 1),
    ("fp_hard_loop_exec.c", 1),
    ("fp_hard_absplit_exec.c", 1),
    ("fp_hard_double_exec.c", 1),
]
HARD_FLOAT_CFLAGS = "-mfloat-abi=hard -mfpu=fpv5-sp-d16"


def _generate_hard_float_params():
    params = []
    ids = []
    for test_file, expected in HARD_FLOAT_TEST_FILES:
        for opt in OPT_LEVELS:
            params.append((test_file, expected, opt))
            ids.append(f"{_test_id(test_file)}_hard{opt}")
    return params, ids


_HARD_FLOAT_PARAMS, _HARD_FLOAT_IDS = _generate_hard_float_params()


@pytest.mark.parametrize("test_file,expected_exit_code,opt_level", _HARD_FLOAT_PARAMS, ids=_HARD_FLOAT_IDS)
def test_hard_float_execution(test_file, expected_exit_code, opt_level, tmp_path):
    cflags = f"{opt_level} {HARD_FLOAT_CFLAGS}"
    _run_qemu_test(test_file, expected_exit_code, opt_level=cflags, output_dir=tmp_path)


# Float ABI x libm interop.  These call real libc/libm functions, which follow
# the program's float ABI (unlike __aeabi_* helpers, which are always base-PCS),
# so the whole program — including libc, libm, libgcc and the C runtime — has to
# be built for one ABI.  float_abi= selects that consistently; passing
# -mfloat-abi as a bare cflag would compile one way and link the other.
_LIBM_ABI_PARAMS = [
    (abi, opt) for abi in ("soft", "softfp", "hard") for opt in OPT_LEVELS
]


@pytest.mark.parametrize("float_abi,opt_level", _LIBM_ABI_PARAMS,
                         ids=[f"fp_libm_{a}{o}" for a, o in _LIBM_ABI_PARAMS])
def test_libm_float_abi(float_abi, opt_level, tmp_path):
    _run_qemu_test("fp_libm_exec.c", 1, opt_level=opt_level, output_dir=tmp_path,
                   float_abi=float_abi)


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
    ("bug_mask_copy_noloop.c", 0),
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
