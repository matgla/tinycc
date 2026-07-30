# Flat (pre-SSA) optimization pass source files — build configuration
# Included by the top-level Makefile.  Landing zone for flat opt_gens_* passes
# relocated out of ir/ onto the source/opt/framework DSL.

# Include paths added to the global DEFINES by the top-level Makefile.
FLAT_OPT_INC = -I$(TOP)/source/opt/flat -I$(TOP)/source/opt/flat/include \
	-I$(TOP)/source/opt/flat/scalar -I$(TOP)/source/opt/flat/cfg \
	-I$(TOP)/source/opt/flat/fusion -I$(TOP)/source/opt/flat/memory \
	-I$(TOP)/source/opt/flat/dce -I$(TOP)/source/opt/flat/loop \
	-I$(TOP)/source/opt/flat/ipa

# Compilation units — appended to IR_FILES in the top-level Makefile.
FLAT_OPT_SRC = \
	source/opt/flat/scalar/bool.c \
	source/opt/flat/scalar/branch.c \
	source/opt/flat/scalar/branch_fold.c \
	source/opt/flat/scalar/stack_bool_diamond.c \
	source/opt/flat/scalar/const_string_calls.c \
	source/opt/flat/scalar/const_call_fold.c \
	source/opt/flat/scalar/addrof_const_fold.c \
	source/opt/flat/scalar/global_init_prop.c \
	source/opt/flat/scalar/symref_const_prop.c \
	source/opt/flat/scalar/complex_const_param_fold.c \
	source/opt/flat/scalar/stack_addr_fold.c \
	source/opt/flat/scalar/single_value_tmp.c \
	source/opt/flat/scalar/call_result.c \
	source/opt/flat/scalar/self_arith.c \
	source/opt/flat/scalar/narrow_store.c \
	source/opt/flat/scalar/self_copy.c \
	source/opt/flat/scalar/var_tmp_fwd.c \
	source/opt/flat/scalar/known_bits.c \
	source/opt/flat/scalar/const_prop_tmp.c \
	source/opt/flat/scalar/value_tracking.c \
	source/opt/flat/scalar/const_var_prop.c \
	source/opt/flat/scalar/const_aggregate.c \
	source/opt/flat/scalar/switch_collapse.c \
	source/opt/flat/scalar/add_reassoc.c \
	source/opt/flat/scalar/cmp_field_fuse.c \
	source/opt/flat/scalar/cmp_expr_fold.c \
	source/opt/flat/scalar/float_narrow.c \
	source/opt/flat/scalar/bitfield.c \
	source/opt/flat/memory/symaddr_cse.c \
	source/opt/flat/memory/dse.c \
	source/opt/flat/memory/var_store_elim.c \
	source/opt/flat/memory/entry_store_prop.c \
	source/opt/flat/memory/sl_forward.c \
	source/opt/flat/memory/copy_source_load_fwd.c \
	source/opt/flat/memory/dead_local_slot.c \
	source/opt/flat/memory/dead_temp_local.c \
	source/opt/flat/memory/dead_static_store.c \
	source/opt/flat/memory/global_base_share.c \
	source/opt/flat/memory/byte_store_merge.c \
	source/opt/flat/memory/mem_inline.c \
	source/opt/flat/fusion/indexed_chain.c \
	source/opt/flat/fusion/pair_reorder.c \
	source/opt/flat/fusion/disp.c \
	source/opt/flat/fusion/gens_fusion.c \
	source/opt/flat/fusion/add_deref_fold.c \
	source/opt/flat/fusion/call_chain_rename.c \
	source/opt/flat/fusion/stackoff_addr_cse.c \
	source/opt/flat/fusion/lea_fold.c \
	source/opt/flat/fusion/lea_rmw_fold.c \
	source/opt/flat/fusion/assign_fuse.c \
	source/opt/flat/fusion/pack64.c \
	source/opt/flat/fusion/pack64_tautology.c \
	source/opt/flat/fusion/cmp_narrow_64.c \
	source/opt/flat/fusion/shl32_or_chain.c \
	source/opt/flat/memory/dead_lea_store.c \
	source/opt/flat/memory/store_inplace_arith.c \
	source/opt/flat/scalar/redundant_loop_check.c \
	source/opt/flat/cfg/if_convert.c \
	source/opt/flat/dce/body_essential.c \
	source/opt/flat/dce/noreturn_collapse.c \
	source/opt/flat/dce/infinite_loop_simplify.c \
	source/opt/flat/dce/trap_only_body_suppress.c \
	source/opt/flat/dce/infinite_self_recursion.c \
	source/opt/flat/dce/noreturn_call_epilogue.c \
	source/opt/flat/dce/dead_addrvar_elim.c \
	source/opt/flat/dce/dead_trailing_addrvar_store.c \
	source/opt/flat/dce/uninit_ub.c \
	source/opt/flat/dce/ub_only_body_elide.c \
	source/opt/flat/dce/const_return_uninit_elide.c \
	source/opt/flat/dce/null_store_dom_return.c \
	source/opt/flat/dce/dce.c \
	source/opt/flat/dce/compact_nops.c \
	source/opt/flat/dce/orphan_cmp_elim.c \
	source/opt/flat/dce/zero_vla_elim.c \
	source/opt/flat/loop/loop_ir_mutate.c \
	source/opt/flat/loop/iv_analysis.c \
	source/opt/flat/loop/iv_strength_reduction.c \
	source/opt/flat/loop/loop_exit_analysis.c \
	source/opt/flat/loop/loop_eliminate.c \
	source/opt/flat/loop/loop_unroll.c \
	source/opt/flat/loop/loop_rotate.c \
	source/opt/flat/loop/loop_relayout.c \
	source/opt/flat/loop/seq_guard_elim.c \
	source/opt/flat/loop/decrement_to_zero.c \
	source/opt/flat/loop/const_sim.c \
	source/opt/flat/scalar/neg_chain_cse.c \
	source/opt/flat/loop/reroll.c \
	source/opt/flat/cfg/switch_to_data.c \
	source/opt/flat/dce/dead_vla.c \
	source/opt/flat/cfg/jump_thread.c \
	source/opt/flat/cfg/jumpif_invert.c \
	source/opt/flat/scalar/var_to_tmp.c \
	source/opt/flat/cfg/returnvalue_merge.c \
	source/opt/flat/loop/backedge_phi_hoist.c \
	source/opt/flat/cfg/abort_tail_merge.c \
	source/opt/flat/loop/loop_detect.c \
	source/opt/flat/loop/func_purity.c \
	source/opt/flat/loop/licm.c \
	source/opt/flat/loop/stack_param_promote.c \
	source/opt/flat/memory/invariant_global_load_hoist.c \
	source/opt/flat/memory/invariant_temp_deref_hoist.c \
	source/opt/flat/memory/global_deref_cse.c \
	source/opt/flat/memory/bitfield_unit_narrow.c \
	source/opt/flat/memory/rmw_byte_clear.c \
	source/opt/flat/memory/local_copy_prop.c \
	source/opt/flat/memory/struct_copy_roundtrip_elim.c \
	source/opt/flat/fusion/barrel_shift.c \
	source/opt/flat/fusion/shift_pair_ubfx.c \
	source/opt/flat/fusion/shift64_dead_half.c \
	source/opt/flat/ipa/pure_via_sret.c \
	source/opt/flat/ipa/func_write_summary.c \
	source/opt/flat/ipa/tu_func_summary.c \
	source/opt/flat/ipa/tu_noreturn.c \
	source/opt/flat/ipa/tu_dead_statics.c \
	source/opt/flat/memory/dead_init_via_call.c \
	source/opt/flat/memory/stack_addr_cse.c \
	source/opt/flat/memory/block_copy_init.c \
	source/opt/flat/memory/small_memset_to_store.c \
	source/opt/flat/memory/small_global_memset_to_store.c \
	source/opt/flat/memory/mem_init.c \
	source/opt/flat/memory/addrof_var_fwd.c \
	source/opt/flat/memory/ptr_local_fwd.c \
	source/opt/flat/memory/memmove_to_indexed_stores.c

# Header set the units depend on (force a rebuild on interface edits).
FLAT_OPT_HDRS = \
	source/opt/flat/ipa/func_write_summary.h \
	source/opt/flat/ipa/tu_summary.h \
	source/opt/flat/include/opt/flat/bool.h \
	source/opt/flat/include/opt/flat/branch.h \
	source/opt/flat/include/opt/flat/call_result.h \
	source/opt/flat/include/opt/flat/self_arith.h \
	source/opt/flat/include/opt/flat/narrow_store.h \
	source/opt/flat/include/opt/flat/self_copy.h \
	source/opt/flat/include/opt/flat/var_tmp_fwd.h \
	source/opt/flat/include/opt/flat/known_bits.h \
	source/opt/flat/include/opt/flat/symaddr_cse.h \
	source/opt/flat/include/opt/flat/indexed_chain.h \
	source/opt/flat/include/opt/flat/pair_reorder.h \
	source/opt/flat/include/opt/flat/disp.h \
	source/opt/flat/include/opt/flat/if_convert.h \
	$(OPT_DSL_HEADERS) \
	$(wildcard ir/*.h) \
	tcc.h tccir.h

$(X)source/opt/flat/%.o: source/opt/flat/%.c $(FLAT_OPT_HDRS) config.mak
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))
