# SSA optimization pass source files — build configuration
# Included by the top-level Makefile.  Landing zone for SSA passes relocated
# out of ir/opt/ (see docs/plan_ssa_const_string_fold.md).

# Include paths added to the global DEFINES by the top-level Makefile.
# The per-domain subdirs are on the path so basename #includes of the passes'
# private headers resolve from ir/, ir/opt/ and the unit tests too.
SSA_OPT_INC = -I$(TOP)/source/opt/ssa -I$(TOP)/source/opt/ssa/include \
	-I$(TOP)/source/opt/ssa/string \
	-I$(TOP)/source/opt/ssa/scalar \
	-I$(TOP)/source/opt/ssa/memory \
	-I$(TOP)/source/opt/ssa/cfg \
	-I$(TOP)/source/opt/ssa/dce \
	-I$(TOP)/source/opt/ssa/loop

# Compilation units — appended to IR_FILES in the top-level Makefile.
SSA_OPT_SRC = \
	source/opt/ssa/string/const_string_fold.c \
	source/opt/ssa/string/str_strlen.c \
	source/opt/ssa/string/str_strcmp.c \
	source/opt/ssa/string/str_strcpy.c \
	source/opt/ssa/string/str_strspn.c \
	source/opt/ssa/string/str_strchr.c \
	source/opt/ssa/string/str_strstr.c \
	source/opt/ssa/string/str_memchr.c \
	source/opt/ssa/memory/global_addr_hoist.c \
	source/opt/ssa/memory/load_cse.c \
	source/opt/ssa/memory/diamond_store_fwd.c \
	source/opt/ssa/scalar/bitop_const_fold.c \
	source/opt/ssa/scalar/strength.c \
	source/opt/ssa/scalar/reassoc.c \
	source/opt/ssa/scalar/narrow.c \
	source/opt/ssa/scalar/fold.c \
	source/opt/ssa/scalar/gvn.c \
	source/opt/ssa/scalar/var_imm_prop.c \
	source/opt/ssa/scalar/cprop.c \
	source/opt/ssa/scalar/setif_or_taut.c \
	source/opt/ssa/scalar/bool_norm.c \
	source/opt/ssa/scalar/cmp_offset_fold.c \
	source/opt/ssa/scalar/tmp_block_const.c \
	source/opt/ssa/cfg/phi.c \
	source/opt/ssa/cfg/branch.c \
	source/opt/ssa/cfg/switch_fold.c \
	source/opt/ssa/cfg/or_bool_diamond.c \
	source/opt/ssa/cfg/cmp_eq.c \
	source/opt/ssa/cfg/vrp.c \
	source/opt/ssa/dce/temp_worklist.c \
	source/opt/ssa/dce/unreachable.c \
	source/opt/ssa/dce/dead_var_stores.c \
	source/opt/ssa/dce/stackloc_stores.c \
	source/opt/ssa/dce/dce_common.c \
	source/opt/ssa/dce/orphan_params.c \
	source/opt/ssa/dce/ret_path_frame_store.c \
	source/opt/ssa/dce/dead_phi_cycles.c \
	source/opt/ssa/dce/dead_overwrite_stores.c \
	source/opt/ssa/dce/dead_global_stores.c \
	source/opt/ssa/dce/var_liveness.c \
	source/opt/ssa/dce/dce.c \
	source/opt/ssa/loop/loop_rotate.c \
	source/opt/ssa/loop/first_iter_exit.c \
	source/opt/ssa/loop/loop_cand.c \
	source/opt/ssa/loop/ptr_iv_exit_subst.c \
	source/opt/ssa/loop/loop_const_sim.c \
	source/opt/ssa/loop/loop_unroll.c \
	source/opt/ssa/loop/iv_strength_reduction.c \
	source/opt/ssa/loop/decrement_to_zero.c \
	source/opt/ssa/loop/dead_loop.c \
	source/opt/ssa/scalar/sccp.c \
	source/opt/ssa/memory/global_store_dse.c \
	source/opt/ssa/memory/ptr_store_dse.c \
	source/opt/ssa/cfg/guard_collapse.c \
	source/opt/ssa/engine/use_def.c \
	source/opt/ssa/engine/rewrite.c \
	source/opt/ssa/engine/stack_resolve.c \
	source/opt/ssa/engine/driver.c

# Header set the units depend on (force a rebuild on interface edits).
SSA_OPT_HDRS = \
	source/opt/ssa/string/str_handlers.h \
	source/opt/ssa/string/const_string_fold.h \
	source/opt/ssa/memory/global_addr_hoist.h \
	source/opt/ssa/memory/load_cse.h \
	source/opt/ssa/memory/diamond_store_fwd.h \
	source/opt/ssa/memory/global_store_dse.h \
	source/opt/ssa/scalar/bitop_const_fold.h \
	source/opt/ssa/include/opt/ssa/strength.h \
	source/opt/ssa/include/opt/ssa/reassoc.h \
	source/opt/ssa/include/opt/ssa/narrow.h \
	source/opt/ssa/include/opt/ssa/fold.h \
	source/opt/ssa/include/opt/ssa/gvn.h \
	source/opt/ssa/include/opt/ssa/phi.h \
	source/opt/ssa/include/opt/ssa/branch.h \
	source/opt/ssa/include/opt/ssa/cmp_eq.h \
	source/opt/ssa/include/opt/ssa/vrp.h \
	source/opt/ssa/include/opt/ssa/setif_or_taut.h \
	source/opt/ssa/include/opt/ssa/bool_norm.h \
	source/opt/ssa/include/opt/ssa/cmp_offset_fold.h \
	source/opt/ssa/include/opt/ssa/tmp_block_const.h \
	source/opt/ssa/include/opt/ssa/var_imm_prop.h \
	source/opt/ssa/include/opt/ssa/cprop.h \
	source/opt/ssa/dce/dce_common.h \
	source/opt/ssa/dce/dce_passes.h \
	source/opt/ssa/loop/loop_cand.h \
	source/opt/ssa/include/opt/ssa/ssa_opt_helpers.h \
	$(OPT_DSL_HEADERS) \
	source/include/tcc.h source/ir/tccir.h source/ir/ir.h source/opt/ssa/include/ssa_opt.h source/opt/include/opt_utils.h

$(X)source/opt/ssa/%.o: source/opt/ssa/%.c $(SSA_OPT_HDRS) $(TCC_CORE_DEPS)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))
