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
    """Expect a line from QEMU output: a whole output line, not text inside one.

    A substring search let test_frame_relayout's wrong "11" pass: its
    expected "10" was found inside the next line, 2000000031000000121.

    If the expected line ends with a float literal (e.g. "sum=3.500000"),
    capture the actual float and compare within tolerance.
    """
    if expected_line is None:
        return
    expected_line = expected_line.rstrip()

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
        pattern = _whole_line("".join(parts))

        sut.expect(pattern, timeout=timeout)
        actual_values = [float(sut.match.group(i + 1)) for i in range(len(expected_values))]
        for expected_value, actual_value in zip(expected_values, actual_values):
            if abs(actual_value - expected_value) > float_tol:
                raise AssertionError(
                    f"Float output mismatch: expected {expected_value} got {actual_value} (tol={float_tol})"
                )
        return

    sut.expect(_whole_line(_escape_regex(expected_line)), timeout=timeout)

MACHINE = "mps2-an505"
CURRENT_DIR = Path(__file__).parent

# Add test files here - each must have a corresponding .expect file
TEST_FILES = [
    ("bug_aggregate_copy_alignment.c", 0),
    ("bug_readint64_combine.c", 0),
    ("bug_wyhash_readint_copy8.c", 0),
    ("bug_licm_field_ptr_hoist.c", 0),
    ("901_ra_spill_swap.c", 0),
    ("900_loop_param_homes.c", 0),
    ("902_escape_copy_call.c", 0),
    ("903_switch_store_to_data.c", 0),
    # ssa:decrement_to_carry — counted-down `for (; n >= K; n -= K)` loops:
    # guard pre-decrement + latch CMP reading the body's counter, so the
    # codegen CMP skip leaves `subs; bcs` latches (memset/memcpy word loops).
    # Pins iteration counts for signed/unsigned/> shapes, empty bodies, the
    # counter-exit-value +K fixup, chained loops on one counter, and the two
    # declination shapes (counter read in body / after loop without fixup).
    ("904_decrement_to_carry.c", 0),
    # flag-neutral windows between a compare and its branch (setif window
    # fuse, two-register remat) and the u8 stack-passed needle param
    # (ra:stack_param_promote narrow widths).
    ("905_setif_window_fuse.c", 0),
    ("906_narrow_local_probe.c", 0),
    ("907_borrowed_header_nrvo.c", 0),
    ("bug_guarded_field_reuse.c", 0),
    ("897_shifted_address_shared_use.c", 0),
    ("896_licm_promoted.c", 0),
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
    ("bug_dead_loop_exit_phi_mid_body_break.c", 0),
    ("bug_first_iter_exit_keeps_pre_exit_body.c", 0),
    ("bug_ivsr_counter_elim_negative_offset.c", 0),
    ("bug_ivsr_use_after_iv_update.c", 0),
    ("bug_ivsr_continue_latch_before_body.c", 0),
    ("bug_loop_unroll_do_while_eq_continue.c", 0),
    ("bug_sizeof_compound_literal_file_scope.c", 0),
    ("bug_init_compound_literal_member_relocs.c", 0),  # init_putv: member relocs use the member offset
    ("bug_inline_replay_scope_binding.c", 0),  # inlined callee names bind at definition, not in the caller scope
    ("bug_patch_type_shared_typedef_array.c", 0),  # patch_type must not complete the shared typedef array Sym
    ("bug_partition.c", 0),
    ("bug_llong_const.c", 0),
    ("bug_init_putv_const_init_capture.c", 0),
    ("bug_vector_expr_init_const_buffer.c", 0),  # const_init buffer valid only for a captured brace list / string
    ("bug_vstore_const_init_control_flow.c", 0),  # a store never re-validates const_init; loop heads and labels forget it
    ("bug_shl32_or_and_all_ones.c", 0),  # shl32_or_chain must not treat 64-bit AND #-1 as low-32 mask
    ("bug_sso_be_bitfield_cross_unit.c", 0),  # big-endian bitfield crossing its demoted unit: be_bp must not go negative
    ("bug_inline_const_arg_param_write_builtin.c", 0),  # always_inline const-arg subst must not fold builtins on a written parameter
    ("bug_sso_be_runtime_byte_order.c", 0),  # big-endian scalar_storage_order members: every runtime load/store is byte-swapped
    ("bug_skip_to_eol_io_buffer.c", 0),  # directive tail straddling the I/O buffer end must be skipped
    ("bug_mul_by_const.c", 0),
    ("bug_mul_compound.c", 0),
    ("bug_ull_mul10_loop.c", 0),
    ("bug_ull_mul10_once.c", 0),
    ("bug_ll_mul10_switch_min.c", 0),
    ("bug_widening_mul64_control_flow.c", 0),
    ("bug_parse_number_64bit.c", 0),
    ("bug_llong_min_const_cmp.c", 0),
    # cmp_narrow_64 proved a loaded operand's high word zero from the definition
    # of the pointer it is loaded through, and compared only the low words.
    ("bug_cmp_narrow_64_deref_pointer_def.c", 0),
    # Inline const-eval folded non-const static elements to their initializers.
    ("bug_inline_eval_static_array_fold.c", 0),
    ("bug_ull_mul_int_accum.c", 0),
    ("bug_bit_builtins_ll_arg_conversion.c", 0),
    ("bug_struct_slot_reuse.c", 0),
    # run_register_coalescing's swap handed the blocker the high half of a live
    # 64-bit pair (r2:r3); the safety scan looked at low registers only.
    ("bug_ra_register_swap_pair_high_half.c", 0),
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

    # 64-bit (x << 8) | (x >> 24) must not fuse into a 32-bit ROR
    ("bug_rotate_fusion_64bit.c", 0),

    # __real__/__imag__ of call results and of non-complex operands
    ("bug_real_imag_rvalue.c", 0),

    # complex int + - * / on _Complex char/short read the imaginary half past
    # the end of the narrow operand (the whole packed pair was cast at once)
    ("bug_complex_narrow_int_arith_operand_parts.c", 0),

    # struct field post-increment in for-loop (spilled lvalue address fix)
    ("bug_struct_field_postinc.c", 0),

    # const char *const global pointer access (YAFF exported symbol section fix)
    ("bug_const_ptr_got_deref.c", 0),

    # union self-cast through typedef should not take the scalar-to-union extension path
    ("bug_union_self_cast_typedef.c", 0),
    # (union U)x must initialise the member of x's type, not the first member
    ("bug_union_cast_member_type.c", 0),
    # a union initializer must not resize a struct member's flexible array type
    ("bug_union_fam_init_shared_struct_type.c", 0),

    # inline asm operands may reuse their own live registers in IR mode
    ("bug_inline_asm_reserved_regs.c", 0),

    # indirect call whose target is also one of its own arguments
    # (`return v(n, v);`): ICE "cannot find safe register to pre-save indirect
    # call target" at -O1/-O2/-Os when the call is a tail call
    ("bug_ra_tail_call_callee_also_argument.c", 0),
    ("bug_ra_tail_call_target_in_arg_reg.c", 0),

    # `register T x __asm("rN")` locals get a vreg (they aliased the previous
    # local's stack slot) -- the shape of Zig's C-backend syscall wrappers
    ("bug_asm_regvar_local.c", 0),

    # values live across inline asm survive its clobbers and pinned operands
    ("bug_asm_clobber_live_values.c", 0),

    # branch narrowing / CBZ fusion: the rehearsal models mid-function returns
    # and refuses ranges holding inline asm
    ("bug_branch_narrow_rehearsal.c", 0),

    # a byte/halfword local read through its own stack slot (after lea_fold)
    # keeps its width -- Zig's 2-byte `!void` error union test
    ("bug_narrow_local_slot_width.c", 0),

    # a direct memmove into a global writes it: mod-ref must resolve the
    # destination at the call site (copy-source forwarding read the new value)
    ("bug_modref_direct_block_copy.c", 0),

    # small aggregates copied as LOAD/STORE chunks instead of __aeabi_memmove
    ("test_small_aggregate_copy.c", 0),

    # spill slots reused after their interval expires (incl. eviction victims)
    ("test_spill_slot_reuse.c", 0),

    # frame objects left unreferenced are dropped, live ones packed (frame.c)
    ("test_frame_relayout.c", 0),
    # frame objects with disjoint lifetimes share bytes (frame.c colouring)
    ("test_frame_colour.c", 0),

    # an array of structs with a VLA member is itself a VLA
    ("bug_vla_struct_array.c", 0),

    # a constant initializer copied from .rodata keeps its narrow fields narrow
    ("bug_block_copy_init_narrow.c", 0),

    # ra:reload_elim keeps its slot->register facts only across ops that touch
    # neither the register nor the slot (BLOCK_COPY, helper calls, ...), and
    # never deletes a volatile load (test_codegen_asm.py counts the loads)
    ("bug_ra_reload_elim_block_copy_implicit_calls.c", 0),
    ("bug_ra_reload_elim_volatile_frame_load.c", 0),
    ("bug_ra_reload_elim_switch_apply_longjmp.c", 0),
    ("bug_entry_store_asm_mem_output.c", 0),
    ("bug_strlen_fold_const_pointer_var.c", 0),
    ("bug_var_tmp_fwd_lea_var0.c", 0),
    ("bug_object_size_loaded_pointer.c", 0),
    ("bug_ra_widen_ijump_forward_exit_backward_reentry.c", 0),

    # an array's address stored into memory keeps the array's initializer
    ("bug_dse_indexed_store_escape.c", 0),

    # a pointer array read through a LOAD_INDEXED (its result used only as a
    # store base) keeps the stores that fill it: dse's write-only addr-TMP scan
    # must not treat a memory read's dest as that address
    ("bug_dse_indexed_load_base_read.c", 0),

    # const_memcpy_to_dest must not drop a local buffer's const fill when a
    # read reaches it through a pointer the isolation scan cannot resolve
    # (a multi-def TEMP from a ternary).
    ("bug_const_memcpy_dest_multidef_ptr_read.c", 0),

    # dead-store elimination bounds reads through an address by its object
    ("test_dse_object_ranges.c", 0),
    ("test_dse_wide.c", 0),

    # loads/stores through a pointer to a frame object become direct slots
    ("test_stack_deref_fold.c", 0),
    ("test_ptr_local_fwd.c", 0),
    ("test_struct_arg_stack.c", 0),
    # struct values more than 32 KiB from the frame base (16-bit operand offset)
    ("test_struct_far_frame.c", 0),
    # __imag__ x = ... on a complex parameter passed on the stack or split
    ("test_complex_param_parts.c", 0),
    # a by-value struct parameter passed on by value (memcpy source bias)
    ("test_struct_param_pass_on.c", 0),
    # a local filled by a whole copy of a by-value struct parameter aliases it
    ("test_param_copy_alias.c", 0),
    # byte/halfword field stores over a zero-filled local word fold into it
    ("test_slot_const_store_fold.c", 0),
    # a function only forwarding its parameters becomes a branch to the callee
    ("test_pure_forward.c", 0),
    # struct stack parts of 3-7 words copied by LDM/STM
    ("test_struct_stack_ldm.c", 0),
    # static functions with a single call site expand at it (bodies deferred to TU end)
    ("test_inline_called_once.c", 0),
    # __builtin_return_address(0) off SP, no frame pointer, in every prologue shape
    ("test_return_address.c", 0),
    # alloca's size stays live after the allocation
    ("test_alloca_live_size.c", 0),
    # frame objects fully rewritten in each loop iteration share bytes; carried ones do not
    ("test_frame_loop_lifetimes.c", 0),
    # a pointer VAR walking a local array has two definitions: nothing is forwarded through it
    ("test_entry_store_var_walk.c", 0),
    # stack-passed parameters read more than once are loaded into registers by the prologue
    ("test_stack_params_in_regs.c", 0),
    # objects declared first and defined later are laid out once; their bytes are not folded early
    ("test_tentative_definitions.c", 0),
    # a small struct returned in r0 is read back masked, and one bool per check fuses into the branch
    ("test_errunion_bool_checks.c", 0),
    # a static function identical to one already generated is dropped for it, unless its address is taken
    ("test_icf_folding.c", 0),
    # a local copied from a call's own struct buffer is that buffer, anywhere in the function
    ("test_call_buffer_copy_alias.c", 0),
    ("test_ldm_block_copy.c", 0),
    ("bug_addr_temp_reuse_before_def.c", 0),
    ("bug_sra_narrow_field_undefined.c", 0),
    ("bug_narrow_field_store_forward.c", 0),
    ("bug_alive_share_evicted_owner.c", 0),
    # ra:branch_thread trusted a `vr <- x` copy made above a loop header that is
    # itself the flag setter; the back edge skips the copy, so every iteration
    # was threaded as if vr == x and the test of vr was dropped (-O1/-O2/-Os)
    ("bug_ra_branch_thread_copy_at_join_setter.c", 0),
    # ra:loop_split placed a loop entry copy `T <- V` before the def group's
    # loop-forced write-back `V <- T2` at their shared insertion point, so T
    # read V with no reaching def (-O2)
    ("bug_ra_loop_split_entry_copy_order.c", 0),
    # word copies between frame slots fused into LDM/STM chunks
    ("test_frame_block_copy.c", 0),
    ("test_sra.c", 0),
    ("test_narrow_call_ext.c", 0),
    ("test_cross_jump.c", 0),
    ("test_switch_case_merge.c", 0),
    ("test_self_store.c", 0),

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

    # cmp_expr_fold folds (x + K) == (x + K) to true when x was reassigned
    # between the two adds: the fallback only compared base vregs, pre-SSA.
    ("bug_cmp_expr_fold_base_redef.c", 0),

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
    # token-keyed -O2 function caches must not outlive their file: a later
    # file's calls folded to an earlier file's constants
    (["721_tu_function_caches.c", "721+_tu_function_caches.c"], 0),
    ("../tests2/105_local_extern.c", 0),

    # __builtin_classify_type tests
    ("140_builtin_classify_type.c", 0),

    # __builtin_bswap16, __builtin_bswap32, __builtin_bswap64 tests
    ("145_builtin_bswap.c", 0),

    # Signed constant arguments must be converted to the builtin's unsigned
    # parameter width before constant folding, just like runtime arguments.
    ("bug_bswap_signed_const.c", 0),

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
    ("bug_mem_inline_named_pointer.c", 0),
    # an 8-byte memcpy through a local's address (the Zig backend's align(1)
    # load/store) must expand to plain accesses, not stay a call
    ("bug_memcpy_local_copy8.c", 0),
    # a by-value align(1) 4-byte struct copy read back byte-wise must keep the
    # bytes in registers, not round-trip each group through stack slots
    ("bug_eqlbytes_stack_roundtrip.c", 0),
    ("bug_eqlbytes_kernel_roundtrip.c", 0),
    ("bug_kernel_eql_slice.c", 0),
    ("bug_inline_shared_return.c", 0),

    # find_call_scratch must not hand out r7 (frame base, never a liveness
    # interval) as a data scratch while marshaling stack-passed struct args
    ("428_callsite_struct_fp_clobber.c", 0),

    # SSA phi placement must record a LEA's dest as a fresh def; a pointer
    # var (p=NULL; if (c) p=&x;) lost the &x def at the join and *p folded
    # to a NULL deref at every -O level
    ("429_ssa_lea_dest_phi.c", 0),

    # Frontend backedges (gjmp_addr) must mark their target is_jump_target;
    # a fallthrough-only loop condition lost its TEST_ZERO to setif_fuse and
    # the continue path spun forever on the same bitmask bit
    ("430_backedge_jump_target_fuse.c", 0),

    # Barrel-shift fusion must preserve dereferenced consumer operands and
    # must not move a dereferenced shift source across an intervening store.
    ("431_barrel_shift_lval_operands.c", 0),

    # A param's implicit entry def means a conditional overwrite is never its
    # single def; cmp_expr_fold folded post-assign compares of a stack param
    # to the assigned constant (dropped barrel shifts in the native tcc)
    ("431_param_conditional_zero_singledef.c", 0),

    # struct-valued ternary: roundtrip-elim resolved the copy's address temp
    # through only the latest textual def and deleted a real cross-slot copy
    ("432_struct_ternary_roundtrip_elim.c", 0),

    # global_addr_hoist must not rewrite STRUCT symref operands: the vreg
    # replacement drops the split-encoded ctype_idx and the callsite marshals
    # a by-value struct global with a garbage type
    ("433_struct_byval_global_addr_hoist.c", 0),

    # inline expansion must retarget the struct return slot only ONCE; a
    # second `return <local>;` moved the caller's read off the first return's
    # slot, whose value was never copied (uninitialized read)
    ("434_inline_two_returns_struct_slot.c", 0),
    ("bug_inline_struct_return_fallback_then_local.c", 0),

    # 64-bit/FP values must not be if-converted to SELECT: the single-register
    # ITE lowering keeps garbage in the high word (self-host: known_bits'
    # width mask got dest_btype as its high word, collapsing all i64 folds)
    ("435_llong_select_high_word.c", 0),

    # SCCP must not match anon StackLoc stores against address-taken VARs by
    # original_offset (a creation-time watermark, not a real slot): it forwarded
    # `m.kind = 1` into loads of the out-params sym/off (memloc_of's shape)
    ("436_sccp_addrtaken_var_slot_confusion.c", 0),

    # double->float libcall narrowing must not rewrite `(float)ceil((double)x)`
    # into a SELF-call inside ceilf's own definition (libm wrappers collapsed
    # to `b .` self-loops via infinite_self_recursion; device fp hang)
    ("437_libcall_narrow_self_recursion.c", 0),

    # VRP must not resolve a loop-carried phi operand (prev = last iter's t)
    # against ranges scoped to the current iteration's guards; it folded
    # `prev == K` checks away (tccgen's prescan lost its &&-parent-label
    # handling: nested-fn label torture tests failed to compile)
    ("438_vrp_loop_phi_carried_range.c", 0),

    # var_tmp_fwd must not forward off `V***DEREF*** <- T`: that stores T
    # THROUGH the pointer V, it does not define V. Later reads of the pointer
    # were rewritten to the stored value, so `*(rr = *rd) = ++cnt; rr[i] = x;`
    # emitted the indexed stores with cnt as the base register (toybox
    # save_redirect wrote its redirect undo list to absolute address cnt+off,
    # leaving it zeroed -> unredirect ran dup2(0,0)+close(0) on the shell)
    ("439_assign_expr_pointer_base.c", 0),

    # dom-LICM may hoist an invariant read of a local's OWN stack slot
    # (`StackLoc[off]`, not a pointer deref) out of a loop that stores to
    # globals, provided the slot's address escapes no further than a
    # non-capturing mem* helper outside the loop -- the shape a small struct
    # assignment lowers to (`T = &cur; __aeabi_memmove4(T, &queue[h], 12)`).
    # Guards both directions: the address escaping to a global, and a write
    # through a pointer taken inside the loop, must still block the hoist.
    ("443_licm_invariant_slot_read.c", 0),

    # IV strength reduction reaching an INNER loop, over indexed loads/stores.
    # Both were gated off: the transform refused LOAD_INDEXED/STORE_INDEXED
    # uses ("the backend already forms efficient indexed addressing" -- measured
    # false on Cortex-M33: a scaled register offset costs +1 cycle over a plain
    # base at equal instruction count), and the driver only ever considered
    # outermost loops, while every hot array walk is nested.  Each case fails
    # with a wrong sum, not merely slower code, if the pointer walk desyncs
    # from the index it replaced.
    ("444_iv_ptr_walk_inner_loop.c", 0),

    # Post-allocation copy propagation and redundant reload elimination
    # (source/ir/regalloc.c).  Both rewrite or delete instructions that already
    # name physical registers, so an error is a silent wrong value rather than
    # a crash, and the rest of the corpus never produces their shape: one
    # union-punned 64-bit value re-read by a dozen inlined accessors across
    # branches, which is how every soft-float routine opens.  Also pins the
    # cases they must REFUSE -- a source redefined under the copy's uses, and a
    # punned slot whose address escapes to a callee that rewrites it.
    ("445_postra_copy_prop_reload.c", 0),

    # shift64_dead_half's annotation is only valid against the FINAL
    # instruction stream (source/opt/flat/fusion/shift64_dead_half.c).  It ran
    # before register allocation, so a later CSE could give the shift a second
    # reader after a half had been declared dead -- codegen then skipped
    # emitting that half and the consumer read an uninitialised register.  A
    # silent wrong value with no crash, and nothing else in the corpus has the
    # shape: two readers of one 64-bit shift, one per half.
    ("446_shift64_dead_half_two_uses.c", 0),

    # setif_branch_remat rewrites branch CONDITIONS, so a mistake is a silently
    # inverted or skipped branch rather than a crash -- and the whole corpus
    # stayed green while an earlier version of it miscompiled a parameter
    # reassigned between the comparison and the branch.
    ("447_setif_branch_remat.c", 0),

    # 64-bit shift lowering: the ORR-folded cross term, the variable-count
    # sequence, and its zero-high-word specialization.  A dropped shift field
    # or a mis-disabled cross term is a wrong value with no fault.
    ("448_shift64_lowering.c", 0),

    # cmp_narrow_64 rewrites a compare's operand WIDTH.  A signed order on a
    # zero-high-word value must keep 64 bits (positive at 64, negative as
    # int32), and a named local needs one definition and no escaping address.
    ("449_cmp_narrow_64.c", 0),

    # Every loop tcc rotates ends up bottom-tested, and ssa:dead_loop only
    # matched the un-rotated shape (CMP first in the header) -- so it fired on
    # nothing real.  The rotated arm kills the back-edge; the negatives here
    # each trip one of its conditions (trip-count-dependent exit value, an
    # escaping counter, a store, a second back-edge, a volatile read).
    ("450_dead_loop_rotated.c", 0),

    # ssa:switch_fold picks the arm of a constant-selector SWITCH_TABLE at
    # compile time.  What it has to get right is every way a selector can miss
    # the table -- below min, above max, no default arm, a negative min -- plus
    # arms that fall through and arms with a side effect.
    ("451_switch_const_selector.c", 0),

    # A folded strcpy copies exactly strlen+1 bytes, which need not be a whole
    # number of words -- the 1..3 byte tail goes as bytes so nothing past the
    # terminating NUL is touched.
    ("452_strcpy_subword_tail.c", 0),

    # That fold turns the call into a BLOCK_COPY over the destination range.
    # Passes that forward a stored value to a later load must treat it as a
    # killing write; three of them did not, and reads came back with bytes the
    # copy had already overwritten (sccp, known_bits, entry_store_prop).
    ("453_block_copy_clobbers_stale_store.c", 0),

    # strcpy from a frame buffer with known contents copies from the rodata the
    # contents came from.  The cases here are the reasons to refuse: the buffer
    # is rewritten in the loop, an opaque callee can write through it, or it is
    # itself a copy destination.
    ("454_strcpy_stack_const_source.c", 0),

    # zero_half64 tells codegen which halves of a 64-bit value are provably the
    # constant zero, and which halves of a result no consumer reads and so need
    # not be written -- a wrong answer leaves a register unwritten and whatever
    # the allocator left in it is read as the value.  The refusals matter as
    # much as the rewrites: an immediate second operand, a narrow destination,
    # and a value assigned twice.
    ("455_zero_half64.c", 0),

    # `(v64 >> k) & mask` with k >= 32 as one UBFX on the high word -- every
    # accessor in lib/fp/soft/soft_common.h.  Sweeps every count and width, and
    # pins the refusals: a field straddling the word boundary, a count below
    # 32, an arithmetic shift, a shift read twice, a source rewritten in
    # between.
    ("456_shift64_extract_ubfx.c", 0),

    # ra:retarget_producer makes the instruction before a register copy write
    # the copy's destination and deletes the copy, then rewrites the reads of
    # the source that outlive it.  Both directions of that are physical-register
    # edits, so what has to hold is the refusals: an ABI-fixed destination, a
    # label between producer and copy, a source read again as an address, a
    # destination redefined under a redirected read, and the 64-bit pair form.
    ("457_retarget_producer.c", 0),

    # The encoder's frame-slot reload cache reaches the 64-bit pair forms: a
    # `ldrd rA,rB,[slot]` right after the `strd rA,rB,[slot]` that filled them
    # is not emitted.  The cases that matter are the ones where the slot or a
    # register changes underneath -- through a pointer, through a callee, one
    # word at a time, or read back as volatile -- where a skipped load returns
    # the value from before the change.
    ("458_ldrd_after_strd.c", 0),

    # global_base_share re-bases a store onto a neighbouring global's address,
    # so `b = 1` resolves as (sym=a, off=4) while a read of `b` resolves as
    # (sym=b, off=0).  The dead-store pass compared those by Sym* and dropped a
    # store whose value was still being read; it compares linker addresses now.
    ("459_global_base_share_alias.c", 0),

    # cmp_imm_swap exchanges the operands of a `CMP #k, reg` so the constant
    # lands in `cmp`'s immediate slot, which is only correct if the condition is
    # mirrored on every reader of the flags.
    ("460_cmp_imm_swap.c", 0),

    # One level of a recursive function is expanded into itself at -O1/-O2.
    # Static instruction counts cannot tell a correctly guarded expansion from
    # one whose copy escaped the guard, so count the side effects at run time.
    ("461_self_inline_side_effects.c", 0),

    # ssa:switch_fold indexed the frontend's already-normalized selector with
    # the raw case value, so a constant `switch` over a table whose lowest case
    # is not 0 ran the wrong arm.
    ("462_switch_fold_nonzero_min.c", 0),

    # Loop rotation relocates the body; a SWITCH_TABLE's arm indices live
    # outside the instruction stream and were left pointing at the old slots.
    ("463_rotate_switch_in_body.c", 0),

    # loop_relayout matched only a single-test `CMP; JUMPIF; JUMP` header, so
    # any short-circuit loop condition kept both trampoline jumps.  Covers the
    # multi-test header shapes plus the edges the permutation has to remap:
    # a `||` test branching into the body, continue/break, nesting, siblings.
    ("464_relayout_multitest_header.c", 0),

    # The linear scan models a value as one [start,end] range, so a value read
    # in only ONE arm of a diamond looks live in the other and a temp defined
    # there spilled with registers going spare.  ra:alive_share hands it a
    # register whose owner is provably dead across its range; these are the
    # shapes where that would be a wrong-VALUE bug if the liveness were wrong.
    ("465_alive_share_regalloc.c", 0),

    # licm_global_load's gate was "does the loop write ANY global", so a loop
    # filling one global array reloaded an unrelated global scalar every
    # iteration.  A store to `&G + <variable>` cannot reach another object;
    # a CONSTANT offset can, because global_base_share re-bases a store onto a
    # neighbour's address.  Both directions are covered here.
    ("466_licm_global_load_alias.c", 0),

    # Scaled derefs reduced to a pointer walk that ends post-indexed
    # (iv_scaled_deref + ra:load_postinc/ra:store_postinc): correctness pins
    # for walk/index desync, write-back register, and fused store placement.
    # (Shipped with the pass but missed from this list.)
    ("467_scaled_deref_ptr_walk_postinc.c", 0),

    # memmove_to_indexed_stores relocated a covered temp's stores onto the
    # memcpy DESTINATION as anonymous StackLoc writes.  When that destination
    # is a NAMED local (the inlined memcpy type-pun shape), the anonymous
    # stores are invisible to every name-keyed analysis and the anonymous
    # StackLoc DCE deletes them, leaving the named load reading uninitialized
    # frame — the self-host break where the device tcc rejected every 'ldr'.
    # The relocated stores must keep the destination var's vreg identity.
    ("468_memmove_fold_named_dst.c", 0),

    # A clamp diamond whose bound phi copy is identity-elided by
    # post_ra_forward_diamond (phi_pinned share): ra_copy_propagate /
    # ra_retarget_producer must not delete or retarget the pinned def, or the
    # shared register enters the loop holding a stale pointer and the loop
    # bound becomes an address (the 04_for.c on-device self-host HardFault).
    ("469_phi_pinned_copy_prop.c", 0),

    # loop_relayout runs to a fixpoint, and its own previous iteration leaves
    # the body's early-exit block between the trampoline and the back edge.
    # The leading latch gap is treated as dead NOPs -- never copied into the
    # permuted buffer, folded onto the increment by relayout_map -- so that
    # live `return` was deleted and every branch to it retargeted at the loop
    # increment.  Needs a dense (contiguous-case) switch to reach the second
    # iteration; this is dce_var_liveness's own bail-out shape, whose loss made
    # the self-hosted tcc drop stores to nested-function captures.
    ("470_relayout_live_latch_gap.c", 0),

    # deref_operand_cse / global_deref_cse end their region at every entry
    # point, but the scan skipped NOPs FIRST -- and a branch target is an
    # index, whose instruction earlier passes routinely blank.  The region then
    # spanned the join and the merge block read a CSE temp only the
    # fall-through predecessor had defined.  This is lcs_try_candidate's own
    # eff_start/eff_end shape, which is why the self-hosted tcc folded away
    # loops that should have run (74 of 78 QEMU failures, 2026-08-23).
    ("471_deref_cse_nop_join.c", 0),

    # A 64-bit compare against a constant whose low half is zero compares only
    # the high words -- one CMP instead of a CMP and a borrow-folding SBCS.
    # Exact for strict order only, so the test carries the refusals (>, <=,
    # ==/!=, a non-zero low half, the constant on the left) beside the wins,
    # and ties -- equal high words, non-zero low word -- in every seed.
    ("525_cmp_hi_only.c", 0),
    ("526_bottom_test_ptr_walk.c", 0),
    # &&/|| whose last operand is a constant: the chain decides, no stale flags
    ("527_landor_const_last_operand.c", 0),

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
    ("../tests2/136_character_constant_values.c", 0),

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
    ("nested_capture_param_stack.c", 0),
    ("nested_capture_param_reg_noinline.c", 0),
    ("nested_capture_param_byref.c", 0),
    ("nested_capture_param_mixed.c", 0),
    ("nested_capture_64bit.c", 0),
    ("nested_capture_two_level.c", 0),
    ("bug_nested_capture_prescan_shadow_scope.c", 0),
    ("sl_forward_load_redefines_var.c", 0),
    ("466_alive_share_boundary_coholder.c", 0),
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
    # Bug: a wide unsigned long long bit-field wrongly truncated the result of
    # ops with a plain 64-bit operand (Y + s.x, Y << s.x, Y += s.x) and of
    # shifts where the bit-field was only the shift count.  gen_op_impl masked
    # whenever *either* operand was a wide unsigned bit-field, ignoring the
    # other operand's width and the shift's left-only typing.  Wrong at all -O.
    ("bug_wide_ull_bitfield_trunc.c", 0),
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

    # The call-crossing 64-bit pair fallback in ra_linear_scan evicted a
    # callee-saved victim without asking whether an alive_share co-holder still
    # lives in its register (x owns r11 across a branch arm where it is dead, `b`
    # borrows it); the freed register went to the pair / the next call-crossing
    # value and overwrote `b`.  Wrong at -O1/-O2/-Os, correct at -O0.
    ("bug_ra_pair_eviction_shared_holder.c", 0),

    # Promoted from orphan triage: builtins, _Complex, aggregate init,
    # 64-bit ops, cast/bitfield, and previously-fixed bug regressions.
    # Verified against the gcc -m32 -funsigned-char oracle.
    ("141_builtin_signbit.c", 0),
    ("142_builtin_copysign.c", 0),
    ("150_builtin_setjmp.c", 0),
    ("160_builtin_prefetch.c", 0),
    # __builtin_signbit parsed its argument in "no code" mode, so the runtime
    # path tested a stale value, dropped the argument's side effects, and died
    # on an argument with no value at all (a call).
    ("bug_builtin_signbit_nocode_arg.c", 0),
    # __builtin_fabs / __builtin_fmax take a double parameter: an int / long
    # long argument reached the libm helper unconverted (garbage high word).
    ("bug_builtin_fabs_int_arg.c", 0),
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

    # #pragma pack around bodies saved as tokens (static inline, -O1 deferred
    # and called-once bodies): the token after a body's '}' was read with the
    # body still capturing, so `f(){} #pragma pack(1) struct S` laid S out
    # unpacked; replays now use the pack state at the body's definition.
    ("472_pragma_pack_saved_bodies.c", 0),

    # A signed add that absorbed an unsigned (wrapping) inner add kept the
    # no-overflow assumption: `(int)(x + 1U) + 1 < (int)x` folded to false.
    ("473_reassoc_unsigned_into_signed.c", 0),

    # static inline bodies a call site did not expand were parsed after the
    # end-of-TU late_reopt fold and dead-static analysis: a static written only
    # there folded to 0, a store read only there was dropped.
    ("474_late_reopt_owed_inline_bodies.c", 0),

    # -fdrop-unused-statics: forward-declared const objects, unsized arrays,
    # liveness through function-pointer tables, block-scope prototypes, used
    # and alias targets -- all deferred to the end of the TU and still defined.
    ("475_drop_unused_statics.c", 0),

    # Write-only statics lose their stores and their bytes, but only when no
    # path reads them: through a call, a data pointer, a function table, or a
    # struct copy.  Stores the frontend's addrtaken used to protect.
    ("476_write_only_statics.c", 0),

    # Frame copy coalescing: a struct copy whose source dies and whose
    # destination is born at the copy shares one slot and the copy goes; a
    # source read later or a destination live before must keep theirs.
    ("477_frame_copy_coalescing.c", 0),

    # __atomic_load/__atomic_store of 1/2/4 bytes are inline LDR/STR with a
    # DMB per the memory order -- no runtime helper (libtcc1 has none).
    ("478_inline_atomic_load_store.c", 0),

    # DSE: a copy of a temp holding one of two stack addresses is still an
    # address; loads through it keep the stores that filled both objects
    # (self-hosted tcc crashed on every 64-bit op in data_processing_mop_impl).
    ("479_dse_ambiguous_addr_copy.c", 0),

    # fputs/printf lowering (fwrite, puts) only when the call is a whole
    # expression statement: `x = printf(...)` and `int r = fputs(...)` keep
    # their value (YasOS libc puts returned 1 for every string).
    ("480_fputs_printf_result_used.c", 0),
    # An address-taken char/short local passed with no promotion (variadic,
    # or to a char/short parameter) is read from its home at its own width,
    # not as a word with the slot's stale upper bytes.
    ("481_narrow_local_arg_width.c", 0),
    # `s = f(s)` with `s` a by-value struct parameter in the argument area:
    # the sret pointer is &s, not the struct's first word.
    ("482_sret_into_struct_param.c", 0),
    # A u8/u16 temporary the allocator spills is stored as a word: its reload
    # is a word load, and a byte store left stale bytes for it (pr82524).
    ("483_narrow_temp_spill_word.c", 0),
    # `f(c ? g() : h())` with a struct result: every word of the selected
    # struct reaches f, not just the first (dead_temp_local).
    ("484_struct_ternary_arg_copy.c", 0),
    # A frame over 32 KiB: colouring keeps an object a struct operand names
    # where it is, since the operand's 16-bit offset would wrap below -32768.
    ("485_frame_colour_struct_far.c", 0),
    # An address-taken local whose address leaves through a call result or a
    # store keeps its slot to the function end (Zig autoHash of a u8, -O0).
    ("486_addrtaken_escape_slot.c", 0),
    # A tail-call-only function with an earlier return still gets its epilogue
    # (InternPool.Alignment.max ran into the next function).
    ("487_tail_call_early_return.c", 0),
    # SCCP does not take `&VAR`'s spill placeholder for a frame slot: a memset
    # through &t0 read as a 0 stored to the spilled by-value parameter.
    ("488_sccp_var_addr_not_slot.c", 0),
    # Post-RA reload elimination pins the dropped load's destination and the
    # stored value together; codegen's scratch fixup moved one of them away.
    ("489_reload_elim_pins_regs.c", 0),
    # dse follows a frame object's address through pointer arithmetic into a
    # VAR; the stores filling the object died while it was read through it.
    ("490_dse_addr_through_var_add.c", 0),
    # A frame word-copy run whose loads share one register is not proof the
    # last word dies: it was read again after the LDM/STM block copy.
    ("491_block_copy_last_word_live.c", 0),
    # A struct-copy round trip whose temporary is also read through its
    # address elsewhere is not a private round trip.
    ("492_struct_roundtrip_addr_escape.c", 0),
    # SCCP does not take a direct VAR store's spill placeholder for a frame
    # slot either (CaptureValue.wrap's case 0 payload folded to 0).
    ("493_sccp_var_store_not_slot.c", 0),
    # dead_addrvar deletes STORE_INDEXEDs through a dead local's LEA along
    # with the LEA; left behind they stored through an undefined register.
    ("494_dead_addrvar_store_indexed.c", 0),
    # A stack-copied struct argument whose base scratch is LR loads its words
    # through another register, and not through a live IP.
    ("495_stack_struct_arg_base_in_lr.c", 0),
    # frame_colour follows a frame address through `T <-- V [LOAD]` (a VAR's
    # value), so storing it escapes the object it points into.
    ("496_frame_colour_ptr_via_var_load.c", 0),
    # The ARMv8-M runtime: arm_mem.S's memcpy/memmove/memset at every
    # alignment, tail length and overlap, and the 64-bit division.
    ("498_aeabi_mem_div_runtime.c", 0),
    # ra:redundant_cmp takes no flags from a compare codegen makes a CBZ (sets
    # none) or from a 64-bit compare (lowered per reader: ORRS for != 0).
    ("499_redundant_cmp_cbz_i64_flags.c", 0),
    # An asm statement clobbering LR (a `bl` in it) makes its function save LR.
    ("500_asm_lr_clobber_is_a_call.c", 0),
    # Small aggregates copied through a pointer held in a local go inline;
    # a struct returned into a local is not copied onto itself.
    ("501_struct_copy_through_pointer_local.c", 0),
    # ssa:dead_loop's vinfo growth left spare entries claiming instruction 0 as
    # their definition; ssa:dce deleted it (a loop counter's initial value).
    ("502_dead_loop_vinfo_grow_def_instr.c", 0),
    # run_register_coalescing's swap left the per-vreg allocation stale;
    # ra:reload_elim deleted a reload into a register the swap had moved.
    ("503_coalesce_swap_stale_allocation.c", 0),
    # sra: 64-bit fields become VARs, a doubleword also touched a word at a
    # time two word VARs (PACK64 to read it whole, split stores); sccp folds a
    # PACK64 of constants and ssa:fold cancels pack/split chains.
    ("504_sra_wide_doubleword_fields.c", 0),
    # 64-bit and narrow-constant slot STOREs are SSA defs; a local only ever
    # stored one constant reads as it (inlined `bits`, int64_t parameters).
    ("505_const_store_var_forward.c", 0),
    # A constant-size __aeabi_memmove4/8 struct copy of up to 128 bytes is
    # emitted inline as LDM/STM; a packed-member copy names plain memmove.
    ("506_inline_aligned_struct_copy.c", 0),
    # UMULL/SMULL fold like MUL: by 0 to 0, both-constant to the product (low
    # words, zero-/sign-extended); zig.h's u128 cross terms on a widened u64.
    ("507_widening_mul_fold.c", 0),
    # UMULL + zero-extended words -> UMAAL before RA (opt/ra/umaal_fusion.c):
    # one/two addends, word views of pairs, loads, bignum row, pair pressure.
    ("508_umaal_fusion.c", 0),
    # A byte/halfword STORE into a register temp is a whole def (MOV, or a
    # word store of the extended value when spilled): not live from entry.
    ("509_narrow_temp_store_def.c", 0),
    # x & M / UBFX x,#0,#w is a copy when x cannot hold a bit outside M:
    # unsigned narrow loads, UBFX fields, temp STORE copies; signed keeps it.
    ("510_known_zero_extend_fold.c", 0),
    # SETIF+TEST_ZERO+JUMPIF fused again after phi resolution: the Zig CBE's
    # one reused `bool t` becomes one single-use temp per check under SSA.
    ("511_late_setif_branch_fuse.c", 0),
    # Pointer-returning helpers inline inside another expansion (Zig CBE
    # header()/acquire() under view()); 930725-1's ternary shape, nested.
    ("512_nested_inline_ptr_return.c", 0),
    ("513_param_across_block_copy.c", 0),
    ("514_inline_large_copies.c", 0),
    ("515_const_local_table_rodata.c", 0),
    ("516_symref_struct_arg_from_table.c", 0),
    ("517_sra_pointer_var_roots.c", 0),
    ("518_ssa_promote_entry_and_sra_fields.c", 0),
    ("519_sra_struct_arg_words.c", 0),
    ("520_sra_copy_and_result_buffer.c", 0),
    ("521_sra_struct_arg_words_large.c", 0),
    ("522_unroll_zig_loop_tables.c", 0),
    ("523_zig_accessor_chain_collapse.c", 0),
    ("522_sra_narrow_field_read_wider.c", 0),
    ("524_sra_far_struct_arg.c", 0),
    ("528_sra_partial_fields.c", 0),
    ("529_sra_pair_copy.c", 0),
    ("530_cbz_over_aligned_branch_target.c", 0),
    ("531_dyn_arg_window.c", 0),
    ("532_unreachable_branches.c", 0),
    ("533_inline_scope_caller_object.c", 0),
    ("534_sccp_entry_store_escaped_call.c", 0),
    ("535_inline_big_struct_param.c", 0),
    ("536_frame_relayout_retry_keeps_copies.c", 0),
    ("537_indexed_chain_deref_base.c", 0),
    ("538_const_local_table_zero_fill.c", 0),
    ("539_cprop_store_copy_inline_helper.c", 0),
    ("540_coalesce_high_pressure_latch.c", 0),
    ("541_param_home_fwd.c", 0),
    ("542_sret_nrvo.c", 0),
    ("543_dead_def_cbz_narrow_ext.c", 0),
    ("544_frame_word_pair_slot_copy.c", 0),
    ("545_cross_jump_alloc.c", 0),
    ("546_ssa_symref_store_def.c", 0),
    ("548_store_imm_scratch_not_base.c", 0),
    # Word-aligned copies of 3..32 words call libtcc1's __tcc_wcopy_N: struct
    # assignment, aligned memcpy, by-value stack arguments (tail call too) and
    # initialiser images in a leaf; every chain entry, guards around the copy.
    ("549_copy_stub_calls.c", 0),
    # byte_store_merge must stop before a sub-word store that is a jump target;
    # otherwise entering at the label skips the merged word store entirely.
    ("550_byte_store_merge_jump_target.c", 0),
    # SRA: a u16 stored into a word field whose other half is struct padding is
    # a plain move (Zig error unions), not a read of the old word and a BFI.
    ("600_sra_padded_partial_store.c", 0),
    # __builtin_{add,sub,mul}_overflow on 8/16/32-bit results: the 32-bit
    # add/sub checks and the int-width narrow path, against 64-bit reference.
    ("601_overflow_builtins_narrow.c", 0),
    # overflow builtins with operands whose sign/width differs from the result:
    # the check uses the operands' own infinite-precision value.
    ("bug_overflow_builtins_mixed_operand_types.c", 0),
    # -Os machine outliner: one template instantiated eight times; its windows
    # (a jump target at the start, a compare feeding the branch after it, an
    # IT block inside) become shared bodies called with BL.
    ("602_outline_shared_windows.c", 0),
    # awk's record reader: a static helper's out-parameter writes to the
    # caller's locals were lost at -O1+, so the whole input was one record
    ("603_inline_outparam_record_split.c", 0),
    # toysh's expand_arg_nobrace: "if (!ant) ant = &deck;" with the deck
    # passed as a stack parameter; "ant != &deck" folded to equal
    ("606_param_ne_local_address.c", 0),
    # toybox awk's logical NOT, "STKP->num = ! get_set_logical();": a
    # comparison still in the flags was converted as its compared operand
    ("607_assign_not_of_call_writing_lhs.c", 0),
    # barrel-shift fusion across a join: a VAR with a def per arm
    ("608_barrel_shift_join_multidef.c", 0),
    # Zig kernel's VFS mount at -O2: `&local + 28` into an inlined interface
    # call; dse dropped the copy into the local
    ("609_dse_inlined_param_var_store_copy.c", 0),
    # Zig kernel's MaxProcFile at -O2: known_bits read `T <-- V [LOAD]` as a
    # load through V, and the vfmt args slice pointer became 2048
    ("610_kb_var_load_is_own_value.c", 0),
    # a weak default is replaceable: never inlined or folded (yaslibc sbrk)
    (["612_weak_default_not_inlined.c", "612+_weak_default_not_inlined.c"], 0),
    # yasld find_thunk at -O2: memcpy into asBytes(&local) then compare
    ("611_find_thunk_memcpy_locals.c", 0),
    # pico-sdk's spinlock release: an asm input whose local the optimizer
    # const-propagated and deleted was still loaded from the dead slot
    ("613_asm_input_const_local.c", 0),
    # frame relayout runs on functions with inline asm: the slot an "=m"
    # operand names stays put while dead and disjoint objects around it go
    ("614_frame_relayout_asm.c", 0),
    # Zig's `self: Self` by value: struct copies whose reads ssa:copy_fwd
    # takes from the source, and the shapes it must leave alone
    ("615_struct_copy_forward.c", 0),
    # ssa:copy_fwd with escapes as events: a copy whose address goes out
    # only later (or in another live range of a reused local) is forwarded;
    # a stashed pointer, a later call or the next iteration still see it
    ("660_copy_fwd_flow_escape.c", 0),
    # ... and with a global as the source: read-only data takes every read,
    # a writable global none after a call or store that may change it
    ("661_copy_fwd_global_source.c", 0),
    # ... and with addresses no operand shows: memcpy/memset's result, a
    # pointer one past the object; a writable variable in a code section;
    # memmove_global_fwd's MLA accumulator and jumps into its window
    ("662_copy_fwd_hidden_addresses.c", 0),
    # frame dead bytes (ir/frame_dfe.c): writes nothing reads dropped, copies
    # shrunk or turned into word moves, objects cut into the pieces still
    # named -- and the escapes, copies out, by-value args it must leave alone
    ("680_frame_dead_bytes.c", 0),
    # a temporary built right before a copy (a 0xaa image, a callee's
    # result) built in the copy's destination instead, and the refusals
    ("681_frame_image_forward.c", 0),
    # ... and against setjmp called by name, computed goto, an asm "m"
    # operand, an overlapping memmove; then the post-RA frame passes it
    # newly reaches: an MLA accumulator read, a reload elimination's
    # register reused by self_store
    ("682_frame_dfe_control.c", 0),
    ("683_post_ra_frame_reads.c", 0),
    # an inlined body's escaped local ends where the body ends -- an index
    # that has to move with the code (unrolled in front of it, copied by an
    # unrolled loop around it), or relayout overlaps it with a live object
    ("684_frame_scope_end_moved.c", 0),
    ("685_frame_scope_end_copies.c", 0),
    # dead_local_slot: a memcpy source through a pointer to one of several
    # locals (a tagged union copied out by value: yasos' SD config) still
    # reads them -- the stores filling them were deleted as dead
    ("686_dead_local_slot_memcpy_src.c", 0),
    # dead_loop: a loop emptied by copy_fwd is deleted up to its exit, and
    # the switch dispatch laid out between its back-edge and that exit went
    # with it -- the body is the natural loop, not the instruction range
    ("687_dead_loop_switch_dispatch.c", 0),
    # an LDM/STM-fused frame word copy used its last word's register as the
    # STM base while ra:reload_elim still read that word from it
    ("688_ldm_stm_fusion_reload_elim.c", 0),
    # memmove_to_indexed_stores: a temporary zeroed by memset then filled,
    # copied through a pointer -- the bytes only the memset wrote (a trailing
    # `0` member) were never stored to the destination (the device tcc's own
    # frame_dfe.c: `d->acc[n] = (DfeAcc){..., 0}`)
    ("689_memmove_indexed_memset_tail.c", 0),
    # docs/bugs round 2026-10-06: a join on a NOP in global_sl_fwd and
    # global_base_share; known_bits taking a loaded pointer for its slot's
    # address; `__imag__ *p`; entry_store, memmove_to_indexed_stores and
    # sl_forward escape/alias holes; reroll's counter init skipped by a branch
    # into the run; computed-goto labels after unrolling and at -O0; narrow
    # integer complex parts (found on the way)
    ("700_global_sl_fwd_nop_join.c", 0),
    ("701_global_base_share_nop_join.c", 0),
    ("702_known_bits_loaded_pointer.c", 0),
    ("703_imag_through_pointer.c", 0),
    ("704_entry_store_escape_chains.c", 0),
    ("705_memmove_stores_before_reads.c", 0),
    ("706_sl_forward_whole_object_escape.c", 0),
    ("707_reroll_entry_through_init.c", 0),
    ("708_computed_goto_labels.c", 0),
    ("709_complex_int_narrow_parts.c", 0),
    # -Os os_const_share: a wide constant (Zig's 0xaaaaaaaa undefined fill, -1,
    # 0x2002) stored, passed and phi-copied at several sites is built once at
    # entry; a spilled share is reloaded, never rematerialised; an entry def
    # in front of a backward-goto target.
    ("560_os_const_share.c", 0),
    # ra:known_ext: extensions of values already extended (loads, masks,
    # phis), duplicate extensions, a BFI that reinserts its own field; and the
    # return classes a direct call to a static function relies on.
    ("580_known_ext_joins.c", 0),
    ("581_known_ext_call_return.c", 0),
    # A widening store into a char/short object (u8 -> u16) casts at once
    # instead of taking vstore's delayed int -> char/short cast.
    ("582_widening_charshort_store.c", 0),
    # Word-by-word copies between frame objects share the two objects' bytes
    # (frame.c frame_find_word_copies): inlined by-value parameters, chains,
    # join objects proved apart by liveness, and the near misses.
    ("590_frame_word_copy_merge.c", 0),
    ("710_memmove_stack_dst_derived_read.c", 0),
    # ssa_string_fold: a pointer loaded from a rodata table is not a string, and
    # strcpy to an odd frame offset must not become a word STM
    ("715_ssa_string_fold_ptr_table.c", 0),
    ("716_strcpy_stack_unaligned_dst.c", 0),
    # toysh's wildcard_matchlen against literal case patterns, and strchr
    # finding the NUL terminator: device-only shapes, pinned
    ("604_toysh_wildcard_literal.c", 0),
    ("605_strchr_nul_terminator.c", 0),
    # a volatile bitfield assignment's value is the value stored, not a
    # re-read of the field (it used to read the register again at -O0)
    ("620_volatile_bitfield_assign_value.c", 0),
    # inline asm "m" operands on locals and stack parameters: addressed off
    # SP/r7 at the final frame offset, and an "m" input's store kept
    ("621_asm_memory_operand_frame.c", 0),
    # an asm operand whose variable was spilled: the lowering resolved its
    # frame home twice and wrote/read below SP (yasos kernel mrs psp), and
    # missed the asm's own register save when addressing off SP
    ("623_asm_spilled_operand.c", 0),
    # a 64-bit equality CMP whose SELECT a tail merge moved behind a JMP took
    # the relational SBCS lowering (gen_opic's compare fold, as the native tcc)
    ("622_cmp64_eq_consumer_behind_jump.c", 0),
    # struct returns built in the caller's buffer across calls (sret_nrvo):
    # the caller never hands a callee a buffer it can reach another way --
    # the destination passed in the same call, stashed, taken in a loop, a
    # pointer (tcc_ir_sret_dealias); locals merged into the buffer only where
    # their lifetimes never meet (gotos, loops, copies both ways); and the
    # front end's claim of a destination only for `x = f(...)` itself, never
    # behind a comma, an argument call, a statement expression, a brace
    # initializer, nor at a packed member's unaligned address
    ("640_sret_noalias.c", 0),
    ("641_sret_nrvo_merge.c", 0),
    ("642_sret_claim_shapes.c", 0),
    ("643_sret_claim_postfix.c", 0),
    ("644_sret_memop_result.c", 0),
    # ... and the copy it adds after a call gets its own orig_index, or a
    # label address on instruction 0 lands on it
    ("645_sret_label_addr_orig_index.c", 0),
    # a pure-forward wrapper whose only call was dropped (the callee folded
    # empty) came out 0 bytes at -O2/-Os and ran into the next function
    ("720_pure_forward_wrapper_dropped_call.c", 0),

    # docs/bugs round 2026-10-06 (2): the nested-function table reallocated
    # under compile_nested_functions; a nested function resolving its
    # siblings' captured offsets against its own IR; captured locals sharing
    # one -O0 slot; dead_vla_struct's nested-function guard reading a field
    # nothing set; a pure-forward wrapper left 0 bytes once its call was dropped
    ("711_nested_funcs_grow_while_compiling.c", 0),
    ("712_nested_sibling_captured_offsets.c", 0),
    ("713_pure_forward_dropped_call.c", 0),
    ("714_nested_captured_slots_whole_function.c", 0),

    # Block-scoped frame objects share bytes across disjoint blocks (frame
    # colouring with block scope ends); a value carried around a loop through
    # a pointer, and a backward goto inside one block, must keep them apart.
    ("740_block_scope_frame_sharing.c", 0),

    # UBSan sweep finds (2026-10-06): a real constant converted to complex long
    # long copied its real part into the imaginary one; out-of-range constant
    # shift counts were folded with the host's UB shift
    ("790_complex_llong_from_real_const.c", 0),
    ("791_shift_count_out_of_range_not_folded.c", 0),

    # ssa:sccp regressions (docs/bugs ssa-sccp-*): escaped frame address and
    # indexed store extents were guessed (4096 / 64 bytes), a narrow store was
    # forwarded without truncation, inline asm and BLOCK_COPY were not writers.
    ("792_sccp_escaped_large_frame_object.c", 0),
    ("793_sccp_indexed_store_large_array.c", 0),
    ("794_sccp_narrow_store_forward_truncates.c", 0),
    ("795_sccp_asm_and_block_copy_writes.c", 0),
    # docs/bugs ssa family: zig counter init moved over an intervening def of
    # the counter; ptr IV exit value substituted past an early break
    ("796_zig_counter_init_clobbered_by_def.c", 0),
    ("797_ptr_iv_exit_subst_early_break.c", 0),

    # docs/bugs round 2026-10-06 (3): dse treated every op off its short
    # blacklist as pure and deleted __builtin_apply / __builtin_setjmp whose
    # result was unused
    ("730_dse_keeps_builtin_apply.c", 0),
    ("731_dse_keeps_builtin_setjmp.c", 0),

    # docs/bugs ssa-store family (2026-10-07): dead_overwrite_stores ignored
    # asm readers, ptr_store_dse ignored __builtin_apply, diamond_store_fwd
    # built a phi-less two-def TEMP (sccp folded it; -O2 crashed)
    ("750_ssa_dos_keeps_store_read_by_asm.c", 0),
    ("751_ptr_store_dse_keeps_store_read_by_apply.c", 0),
    ("752_diamond_store_fwd_two_def_temp.c", 0),

    # gen_c.py "hazard" profile finds (2026-10-06): dead_vla_struct deleted a
    # VLA read only through an inlined call's argument copy
    ("810_dead_vla_read_by_inlined_arg.c", 0),
    # ... and an asm input that is a comparison result was never materialised
    ("811_asm_input_comparison_value.c", 0),
    # ... and an asm output through a pointer lost the pointer
    ("813_asm_output_through_pointer.c", 0),
    # ... ssa:licm hoisted a nested-function-captured local out of a loop
    # calling the writer, and ub_only_body_elide emptied a nested function
    # whose StackLoc (static chain) reads it took for uninitialised locals
    ("814_licm_nested_captured_local.c", 0),
    ("815_nested_store_through_captured_ptr.c", 0),
    # Hazard "sandwich" programs: for each construct, ten shapes of a value
    # read before it, memory changed by it, the value read after (load, expr,
    # compare, global, pure call, loop, local, xor); expected output from
    # arm-none-eabi-gcc -O2.
    ("820_hazard_sandwich_alloca.c", 0),
    ("821_hazard_sandwich_apply.c", 0),
    ("822_hazard_sandwich_asm_clob.c", 0),
    ("823_hazard_sandwich_asm_goto.c", 0),
    ("824_hazard_sandwich_asm_m.c", 0),
    ("825_hazard_sandwich_asm_m_nv.c", 0),
    ("826_hazard_sandwich_asm_pr.c", 0),
    ("827_hazard_sandwich_asm_r.c", 0),
    ("828_hazard_sandwich_call.c", 0),
    ("829_hazard_sandwich_cgoto.c", 0),
    ("830_hazard_sandwich_nested.c", 0),
    ("831_hazard_sandwich_switch.c", 0),
    ("832_hazard_sandwich_vla.c", 0),
    ("833_hazard_sandwich_vstore.c", 0),
    ("834_rodata_relative.c", 0),
    ("835_static_init_element_address.c", 0),
    # __builtin_setjmp resume ADR distance broken by a literal-pool flush
    ("836_builtin_setjmp_after_literal_pool_flush.c", 0),
    ("837_nl_setjmp_after_literal_pool_flush.c", 0),
    # redundant_loop_check folded an in-body test of the guard variable after the body redefined it
    ("838_redundant_loop_check_guard_redef.c", 0),
    # switch_collapse NOPed the dispatch; control fell through to the epilogue
    ("839_switch_collapse_dispatch_fallthrough.c", 0),
    # always_inline + const arg: inline_const_args substituted the param's slot
    # into asm operands regardless of role/def -> wrong value + spurious lvalue error
    ("840_inline_const_arg_asm_param_write.c", 0),
    ("841_gvn_addrtaken_param.c", 0),
    ("843_load_cse_global_pun_store.c", 0),
    ("844_load_cse_narrow_frame_forward.c", 0),
    # ssa:arm_fuse_shl_add_to_{load,store}_indexed read a redefined param at the memory op
    ("842_ssa_shl_add_indexed_param_redef.c", 0),
    # loop passes (source/opt/flat/loop): exit condition, IV init reaching def, unsigned trip count, post-increment IV reads
    ("845_loop_eliminate_symbolic_exit_cond.c", 0),
    ("846_loop_iv_init_reaching_def.c", 0),
    ("847_loop_trip_count_unsigned_cond.c", 0),
    ("848_loop_unroll_iv_read_after_increment.c", 0),
    ("849_tu_modref_memcpy_result_indirect_call.c", 0),
    ("850_temp_slot_call_args.c", 0),
    (["851_weak_callee_modref.c", "851+_weak_callee_modref.c"], 0),
    (["bug_weak_inferred_noreturn.c", "bug_weak_inferred_noreturn+.c"], 0),
    (["bug_weak_pure_via_sret.c", "bug_weak_pure_via_sret+.c"], 0),
    (["bug_func_write_summary_deref_param.c", "bug_func_write_summary_deref_param+.c"], 0),
    ("bug_func_purity_folded_read.c", 0),
    ("bug_pack64_tautology_redef.c", 0),
    ("bug_pragma_pack_push_in_body.c", 0),
    ("bug_post_ra_diamond_switch_target.c", 0),
    ("bug_if_convert_select_goto_switch.c", 0),
    ("bug_sso_be_packed_bitfield_unit_in_struct.c", 0),
    ("852_memset_libc_param_order.c", 0),
    ("853_licm_param_redef.c", 0),
    ("854_local_only_body_phi_pointer.c", 0),
    ("855_local_array_template_conversion.c", 0),
    ("856_libcall_name_static_user_fn.c", 0),
    ("857_label_diff_table.c", 0),
    ("858_hex_float_leading_zeros.c", 0),
    ("859_complex_llong_const_cast.c", 0),
    ("bug_complex_imaginary_literal_uninit_stack.c", 0),
    ("bug_complex_double_div_param_stack.c", 0),
    ("860_alias_to_inlined_static.c", 0),
    ("861_global_block_extern_alias.c", 0),
    ("862_global_sl_fwd_overlap_and_asm.c", 0),
    ("863_dse_pointer_chain_multidef.c", 0),
    ("864_asm_goto_label_only_block.c", 0),
    ("865_complex_llong_runtime_conversions.c", 0),
    # Zig mem.readInt: constant-trip mid-exit byte loop (ssa:loop_unroll_midexit)
    # + byte-chain -> ldr/ldrh (ssa:load_combine), with the shapes both must
    # decline (store between loads, signed bytes, BE, 3 bytes, trip 17, runtime trip).
    ("870_readint_unroll_combine.c", 0),
    # Load CSE keyed on the dest btype instead of the access width/extension:
    # LDRB/LDRSB/LDRH/LDR of one address merged (ssa:cprop, ssa:load_cse,
    # invariant_global_load_hoist); load_combine exposed it on readInt code.
    ("871_load_cse_access_width.c", 0),
    # 2-byte load_combine at base+#k widened to LDR by the LOAD -> LOAD_INDEXED
    # displacement folds; ssa:dead_loop's guarded rewrite dropping exit-live
    # header phis of a loop whose inner constant-trip loop was unrolled.
    ("872_readint_narrow_and_dead_loop.c", 0),
    # return-block register sharing across a branch that leaves the return tail
    ("873_ret_share_branch_exit.c", 0),
    # setif_mask_fold re-emitting / rewriting a barrel-shift-annotated CMP
    ("874_setif_mask_fold_barrel_shift.c", 0),
    ("875_setif_mask_fold_annotated_outer_cmp.c", 0),
    # dead_loop guarded rewrite: a non-candidate header phi read after the loop
    ("876_dead_loop_guarded_phi_escape.c", 0),
    # bool_norm proving {0,1} through an OR whose src2 carries a barrel shift
    ("877_bool_norm_barrel_shift.c", 0),
    # setif_branch_remat re-emitting a barrel-shift-annotated CMP at a branch
    ("878_setif_remat_barrel_shift.c", 0),
    # frame_dfe: copies of never-written bytes and of vregs nothing defines
    # (zig tagged-value temporaries) are dropped; every byte written on some
    # path, through a pointer, by a callee, or volatile still arrives
    ("883_frame_undef_copy.c", 0),
    ("884_errtail_overwritten_ptr_stores.c", 0),
    ("885_errtail_imm_tail_merge.c", 0),
    # ssa:loop_header_dup: while loops with header prefix instructions bottom-tested
    ("880_loop_header_dup.c", 0),
    # ra:jt_backward: conditional branches threaded backwards onto a loop head post-RA
    ("879_loop_backedge_thread.c", 0),
    # ra:backedge_phi_hoist: the exit target must be the fall-through after the latch JUMP
    ("881_backedge_phi_hoist_fallthrough.c", 0),
    # One gen_c.py "hazard" program pinned (seed 233 uses all ten constructs:
    # __builtin_apply on a global and a local, __builtin_setjmp, asm "=m" /
    # "+r" / memory clobber, VLA, alloca, nested function, computed goto);
    # expected checksum from arm-none-eabi-gcc -O0 and -O2.
    ("812_fuzz_hazard_profile_seed233.c", 0),

    # __builtin_constant_p of a comparison (gcc.c-torture/compile/20110902.c):
    # the single-constant-def vreg shortcut read a VT_CMP/VT_JMP value's stale
    # vr (`k < n` -> 1) and its cmp_op/cmp_r union as a Sym pointer (wild heap
    # read, intermittent compiler segfault)
    ("745_builtin_constant_p_comparison.c", 0),
    # __builtin_constant_p of a local reassigned later in a loop body: only the
    # definitions parsed so far were counted, so the wrong 1 was baked into
    # every trip of the loop.
    ("bug_builtin_constant_p_loop_redef.c", 0),
    # __*_chk elision trusted the same flow-insensitive per-Sym max fact: a
    # loop back edge, an escaped address, a pointer write and a run-time
    # objsize each dropped a check the runtime function would have caught.
    ("bug_fortify_objsize_max_fact_flow_insensitive.c", 0),
    # Phase-3 scratch fixup must not split coalesced classes / elided phi ends.
    ("792_scratch_reassign_coalesced_class.c", 0),
    # Stack-passed struct copies save only the registers the call setup still reads.
    ("886_struct_arg_copy_live_regs.c", 0),
    # inline:wrappers / barrier_leaf / trivial_leaf / leaf_rescue: Zig-style accessor chains, capped wrappers.
    ("889_inline_accessor_classes.c", 0),
    # inline:multi3 + arm_mla_zero_product: zig.h's 128-bit multiply without the __multi3 call.
    ("890_inline_multi3.c", 0),
    # arm_addreg_indexed: [Rn, Rm] loads/stores from `base + index` (W4).
    ("891_addreg_indexed.c", 0),
    # ssa:pure_call_cse + purity:transitive: a repeated call to a callee that writes no memory reuses the result.
    ("892_pure_call_cse.c", 0),
    # Read-modify-write atomics through the runtime helpers; the same file runs
    # inline under -minline-atomics below.
    ("893_inline_atomic_rmw.c", 0),
    # System-instruction asm lowered to machine calls, and small loop leaves
    # inlined in expression context.
    ("894_asm_machine_call.c", 0),
    # ra:bump_sink: pointer bumps sunk below the base's last read (post-indexed walks).
    ("895_bump_sink.c", 0),
    # ra:exit_sink: values computed each iteration but read only in a loop-exit
    # block sink into that block (docs/bugs/loop-exit-value-computed-every-iteration.md).
    ("898_loop_exit_sink.c", 0),
    # fuzz seed 2395: the store-source LEA fusion (arm_store_add_imm) hoisted a
    # load to the LICM'd address ADD across a loop back-edge that re-enters
    # after an aliasing store in the same loop (fallthrough join invisible to
    # its linear clobber scan). Wrong only at -O2.
    ("899_fuzz_lea_store_src_loop_hoist.c", 0),
    # ra:caller_save: call-crossing values in caller-saved registers, saved around cold calls.
    ("887_ra_caller_save_cold_calls.c", 0),
    # ra:evict_pair: a cold 64-bit pair held across a hot loop is the victim a single value evicts.
    ("888_ra_evict_cold_pair.c", 0),
    ("891_ra_branch_cost.c", 0),
    ("893_inline_readonly_wrapper.c", 0),
    ("894_pure_call_forward.c", 0),

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
    # RA interval missed a value live over a forward exit + backward re-entry: wrong at all -O.
    ("bug_ra_interval_forward_exit_backward_reentry.c", 0),
    ("bug_ra_interval_live_in_at_asm_block_start.c", 0),
    # codegen ADD/SUB+CMP#0 fusion skipped a CMP that is a loop back-edge target (and a volatile read).
    ("bug_sub_cmp0_fusion_jump_target.c", 0),
    # ra:struct_arg_split: small by-value struct args built in a slot become scalar word PARAMs.
    ("882_struct_arg_word_split.c", 0),
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
    # Bug: ra:load_postinc folded `++p` into the first `*p` load while an
    # outer call still read *p -- its FUNCPARAMVAL sits above the load, but
    # the value is read at the CALL, below it.  printf got p+1 (FatFs
    # f_setlabel -> FR_INVALID_NAME in sdformat).
    ("bug_postinc_call_arg_deref.c", 0),
    # Bug: sl_forward forwarded a frame store across a call although the slot's
    # address escapes later in the same loop body (back edge).
    ("bug_sl_forward_escape_in_loop.c", 0),
    # Bug: switch (a < b) / (!a) / (a && b) dispatched on an operand, not the 0/1 result.
    ("bug_switch_on_comparison.c", 0),
    # Bug: switch_to_data NOPed a case body that a goto still jumps to.
    ("bug_switch_to_data_goto_case.c", 0),


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
    # cast to union from a type no member has is an error, as in gcc
    "bug_union_cast_member_type_errors.c",
    # initializing a struct member's flexible array through a union is rejected
    "bug_union_fam_init_shared_struct_type_errors.c",
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


def _whole_line(pattern):
    """Anchor a pattern to one whole output line (the stream is consumed up
    to each match, so the next line starts right after its newline).  Trailing
    blanks do not count, as in the upstream tests2 harness: several of those
    programs print a space before each newline their .expect files omit."""
    return r"(?m)^" + pattern + r"[ \t]*\r?\n"


def _run_qemu_test(test_file, expected_exit_code, args=None, defines=None, opt_level="-O0", output_dir=None, timeout=10,
                   float_abi=None, extra_objs=None):
    expected_lines, expect_exit = load_expect_file(test_file)
    if expect_exit is not None:
        expected_exit_code = expect_exit
    opt_suffix = f"_{opt_level.replace('-', '').replace(' ', '_')}"
    if float_abi:
        opt_suffix += f"_{float_abi}"
    config = CompileConfig(extra_cflags=opt_level, output_suffix=opt_suffix, output_dir=output_dir,
                           float_abi=float_abi, extra_objs=extra_objs)
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


# ra_widen_intervals_by_liveness falls back to loop-wide widening when its
# per-block bit sets exceed the size cap; lower the cap (debug cross knob) so the
# small reproducer takes that path.
@pytest.mark.parametrize("opt_level", ["-O1", "-O2", "-Os"])
def test_ra_widen_size_cap_fallback(opt_level, tmp_path, monkeypatch):
    monkeypatch.setenv("TCC_RA_WIDEN_MAX_WORDS", "1")
    _run_qemu_test("bug_ra_interval_forward_exit_backward_reentry.c", 0, opt_level=opt_level, output_dir=tmp_path)


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
    ("fp_hard_va_double_arg_exec.c", 1),
    ("fp_hard_forward_float_with_double_exec.c", 1),
    ("fp_hard_unused_double_param_exec.c", 1),
    ("fp_hard_hfa_ret_exec.c", 1),
    ("fp_hard_hfa_args_exec.c", 1),
    ("fp_hard_hfa_ret_regs_exec.c", 1),
    ("fp_hard_complex_exec.c", 1),
    ("fp_hard_vfp_no_backfill_exec.c", 1),
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
    # float_abi="hard" links the hard-float libgcc/libc too: tests that reach a
    # libgcc routine (complex division) need the VFP-convention variant.
    _run_qemu_test(test_file, expected_exit_code, opt_level=cflags, output_dir=tmp_path, float_abi="hard")


# ra:struct_arg_split under the hard-float ABI: the pass changes how by-value
# struct arguments reach the call, and the shapes it rewrites (tuple round trips
# through frame slots) only appear with hard float -- soft float already passes
# them as words.  Same program and expected output as the default run.
@pytest.mark.parametrize("opt_level", OPT_LEVELS, ids=[f"struct_arg_split_hard{o}" for o in OPT_LEVELS])
def test_struct_arg_split_hard_float(opt_level, tmp_path):
    cflags = f"{opt_level} {HARD_FLOAT_CFLAGS}"
    _run_qemu_test("882_struct_arg_word_split.c", 0, opt_level=cflags, output_dir=tmp_path, float_abi="hard")


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


# fp_hard_complex_exec.c under the base-PCS ABIs too: YasOS userspace is softfp,
# where __divdc3 takes a hidden result pointer and the operands in r2:r3 plus
# the stack (a different backend path from the hard-float one above).
_COMPLEX_ABI_PARAMS = [
    (abi, opt) for abi in ("soft", "softfp") for opt in OPT_LEVELS
]


@pytest.mark.parametrize("float_abi,opt_level", _COMPLEX_ABI_PARAMS,
                         ids=[f"fp_complex_{a}{o}" for a, o in _COMPLEX_ABI_PARAMS])
def test_complex_float_abi(float_abi, opt_level, tmp_path):
    _run_qemu_test("fp_hard_complex_exec.c", 1, opt_level=opt_level, output_dir=tmp_path,
                   float_abi=float_abi)


# AAPCS32 interop with gcc-built code: abi_mix_b.c is compiled by
# arm-none-eabi-gcc, main and abi_mix_a.c by tcc, and each side calls the
# other with structs of 17-36 bytes in registers, split and on the stack
# (see abi_mix.h).  The f13 case also puts a struct after 17 VFP arguments;
# under hard-float, f16 has already gone to the stack, so the struct must not
# be split across r3 and the stack (AAPCS32 C.5/C.6).  Build both float ABIs
# explicitly so the hard-float GCC interop path is covered independently of
# the suite default.
_ABI_GCC_INTEROP_PARAMS = [
    (float_abi, opt) for float_abi in ("soft", "hard") for opt in OPT_LEVELS
]


@pytest.mark.parametrize("float_abi,opt_level", _ABI_GCC_INTEROP_PARAMS,
                         ids=[f"abi_gcc_interop_{a}{o}" for a, o in _ABI_GCC_INTEROP_PARAMS])
def test_abi_gcc_interop(float_abi, opt_level, tmp_path):
    import subprocess
    (tmp_path / "gcc").mkdir()
    gcc_obj = tmp_path / "gcc" / "abi_mix_b_gcc.o"  # outside the build dir `make clean` removes
    fp = [f"-mfloat-abi={float_abi}"] + ([] if float_abi == "soft" else ["-mfpu=fpv5-sp-d16"])
    r = subprocess.run(["arm-none-eabi-gcc", "-mcpu=cortex-m33", "-mthumb", *fp, "-O2", "-ffunction-sections",
                        "-c", str(CURRENT_DIR / "abi_mix_b.c"), "-o", str(gcc_obj)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    _run_qemu_test(["abi_mix_main.c", "abi_mix_a.c"], 0, opt_level=opt_level, output_dir=tmp_path / "build",
                   float_abi=float_abi, extra_objs=[gcc_obj])


# Hard-float HFAs against gcc: abi_hfa_b.c is built by arm-none-eabi-gcc
# (-mfloat-abi=hard), main and abi_hfa_a.c by tcc.  HFAs that are not flat
# structs of scalars (arrays, nested structs) must come back in s0/s1/d0 like
# gcc's, larger HFAs and _Complex double in s0-s7 without a hidden result
# pointer, and HFA / _Complex arguments go in s0-s15 (back-filled; on the
# stack once the bank is full, leaving r0-r3 to later core arguments).
@pytest.mark.parametrize("opt_level", OPT_LEVELS, ids=[f"abi_hfa_gcc_interop{o}" for o in OPT_LEVELS])
def test_abi_hfa_gcc_interop(opt_level, tmp_path):
    import subprocess
    (tmp_path / "gcc").mkdir()
    gcc_obj = tmp_path / "gcc" / "abi_hfa_b_gcc.o"
    r = subprocess.run(["arm-none-eabi-gcc", "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-sp-d16",
                        "-O2", "-ffunction-sections", "-c", str(CURRENT_DIR / "abi_hfa_b.c"), "-o", str(gcc_obj)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    _run_qemu_test(["abi_hfa_main.c", "abi_hfa_a.c"], 0, opt_level=opt_level, output_dir=tmp_path / "build",
                   float_abi="hard", extra_objs=[gcc_obj])


# Floating point matrix: every mode-agnostic FP test x FP mode x opt level.
#
# The three modes are the ones userspace ships (docs/userspace_floating_point.md):
#   softfp            FPv5-SP inline for float, double in software, args in GPRs
#   hardfp+softdouble same instructions, AAPCS-VFP (args/results in s0-s15)
#   hardfp+dcp        -mfpu=rp2350: double on the DCP coprocessor
# QEMU's Cortex-M33 has no CP4, so the dcp column cannot execute here; it runs
# on the board through benchmarks/run_fp_conformance.py --matrix.  The fp_hard_*
# tests stay in test_hard_float_execution (they pin the hard ABI itself).
FP_MATRIX_MODES = {
    "softfp": "softfp",
    "hardfp_softdouble": "hard",
}
FP_MATRIX_OPT_LEVELS = ["-O0", "-O1", "-O2"]
FP_MATRIX_TESTS = [
    "test_cleanup_double.c",
    "test_fp_offset_cache.c",
    "test_fp_cache_callee_saved.c",
    "test_double_printf_ops.c",
    "test_double_printf_literals.c",
    "test_double_printf_mixed.c",
    "test_float_math_loop.c",
    "test_float_simple_calc.c",
    "72_float_result.c",
    "73_float_ops.c",
    "test_aeabi_dmul_bits.c",
    "test_aeabi_double_all.c",
    "170_nan_comparison.c",
    "421_fp_conformance.c",
    "428_callsite_struct_fp_clobber.c",
    "223_fuzz_loop_const_sim_fp_compare.c",
    "337_fuzz_genopif_double_round.c",
    "338_fuzz_genopif_double_round2.c",
    "383_fuzz_softfp_cmp_fold_phi_prune.c",
    "390_fuzz_dead_loop_double_phi_imm.c",
    "396_fuzz_fold_double_neg_barrel_shift.c",
    "504_sra_wide_doubleword_fields.c",
    "858_hex_float_leading_zeros.c",
    "fp_libm_exec.c",
]


def _generate_fp_matrix_params():
    expected = {}
    for test_file, exp in TEST_FILES:
        if isinstance(test_file, str):
            expected[test_file] = exp
    expected["fp_libm_exec.c"] = 1
    params, ids = [], []
    for test_file in FP_MATRIX_TESTS:
        exp = expected.get(test_file, 0)
        exit_code, timeout = (exp[0], exp[1] if len(exp) > 1 else 10) if isinstance(exp, tuple) else (exp, 10)
        for mode in FP_MATRIX_MODES:
            for opt in FP_MATRIX_OPT_LEVELS:
                params.append((test_file, exit_code, timeout, mode, opt))
                ids.append(f"{_test_id(test_file)}_{mode}{opt}")
    return params, ids


_FP_MATRIX_PARAMS, _FP_MATRIX_IDS = _generate_fp_matrix_params()


@pytest.mark.parametrize("test_file,expected_exit_code,timeout,mode,opt_level", _FP_MATRIX_PARAMS, ids=_FP_MATRIX_IDS)
def test_fp_matrix(test_file, expected_exit_code, timeout, mode, opt_level, tmp_path):
    float_abi = FP_MATRIX_MODES[mode]
    _run_qemu_test(test_file, expected_exit_code, defines=TEST_FILE_DEFINES.get(test_file), timeout=timeout,
                   opt_level=opt_level, output_dir=tmp_path, float_abi=float_abi)


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
# Tests requiring every function in one .text (-fno-function-sections)
# ---------------------------------------------------------------------------

NO_FUNCTION_SECTIONS_TEST_FILES = [
    # Late reopt re-emitting a function takes the statics folded onto it
    # (identical code folding) along to its new body.  With a section per
    # function the erased body leaves its section empty and the new one lands
    # where the stale alias still points, so the bug needs one shared .text.
    ("497_late_reopt_keeps_icf_alias.c", 0),
]


def _generate_no_func_sections_params():
    params = []
    ids = []
    for test_file, expected in NO_FUNCTION_SECTIONS_TEST_FILES:
        for opt in OPT_LEVELS:
            params.append((test_file, expected, opt))
            ids.append(f"{_test_id(test_file)}{opt}")
    return params, ids


_NO_FUNC_SECTIONS_PARAMS, _NO_FUNC_SECTIONS_IDS = (
    _generate_no_func_sections_params() if NO_FUNCTION_SECTIONS_TEST_FILES else ([], []))


@pytest.mark.parametrize("test_file,expected_exit_code,opt_level", _NO_FUNC_SECTIONS_PARAMS,
                         ids=_NO_FUNC_SECTIONS_IDS)
def test_no_function_sections_bugs(test_file, expected_exit_code, opt_level, tmp_path):
    """Tests compiled into one .text: the harness Makefile passes
    -ffunction-sections, and the -fno- form after it wins."""
    if test_file is None:
        pytest.fail("test_file is None")

    cflags = f"{opt_level} -fno-function-sections"
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
# Tests compiled with -minline-atomics (read-modify-write as LDREX/STREX loops)
# ---------------------------------------------------------------------------

INLINE_ATOMICS_TEST_FILES = [
    ("bug_small_struct_copy_padding.c", 0),
    # Every op, size and order inline; narrow signed values, the failure
    # write-back, a body that is only the atomic (no tail call to a helper that
    # does not exist), and a weak compare-exchange spin loop.
    ("893_inline_atomic_rmw.c", 0),
]


def _generate_inline_atomics_params():
    params = []
    ids = []
    for test_file, expected in INLINE_ATOMICS_TEST_FILES:
        for opt in OPT_LEVELS:
            params.append((test_file, expected, opt))
            ids.append(f"{_test_id(test_file)}{opt}")
    return params, ids


_INLINE_ATOMICS_PARAMS, _INLINE_ATOMICS_IDS = (
    _generate_inline_atomics_params() if INLINE_ATOMICS_TEST_FILES else ([], [])
)


@pytest.mark.parametrize("test_file,expected_exit_code,opt_level", _INLINE_ATOMICS_PARAMS, ids=_INLINE_ATOMICS_IDS)
def test_inline_atomics(test_file, expected_exit_code, opt_level, tmp_path):
    """Tests compiled with -minline-atomics."""
    if test_file is None:
        pytest.fail("test_file is None")

    cflags = f"{opt_level} -minline-atomics"
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

    # A call to a static function defined further down skips the R9 reload.
    ("test_r9_static_forward_call.c", 0),
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
