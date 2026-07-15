# tinycc unit-test binary (target: armv8m)
#
# Builds a host-native executable that links the tinycc module(s) under
# test against a small libtcc stub layer. The target-specific defines
# below match the armv8m cross build so tcc.h parses identically to the
# compiler it validates.
#
# Phase 1 scope: ir/pool.c only. Extend UT_SRCS/UT_OBJS as new suites
# land (Phase 2+).

TOP := $(abspath $(CURDIR)/../../../..)
UT_ROOT := $(abspath $(CURDIR)/../..)

include $(TOP)/ir/gen/Makefile

# Host compiler (do NOT use the armv8m cross compiler).
HOSTCC ?= gcc

# Build directory — all objects and the final binary go here.
BUILD_DIR := build

# Coverage: `make COVERAGE=1 ...` instruments every TU with gcov
# (--coverage == -fprofile-arcs -ftest-coverage), emitting build/**/*.gcno at
# compile time and build/**/*.gcda when the binary runs. The `coverage` target
# (below) does a clean instrumented build, runs the suite, and renders a gcovr
# report. Keep this OFF for normal `make`/`make run` — instrumented objects are
# slower and must not be mixed with un-instrumented ones.
COVERAGE ?= 0
ifeq ($(COVERAGE),1)
UT_COV_CFLAGS := --coverage
UT_COV_LDFLAGS := --coverage
endif

# Coverage-mode stamp. The object rules below track only source timestamps, so
# without this they happily reuse objects compiled in the *other* coverage mode:
# after `make ut-coverage` the build/ tree holds --coverage objects, and a plain
# `make`/`make run` (COVERAGE=0) would relink them un-instrumented, leaving
# gcov's _sub_I_*/__gcov_init ctors unresolved (and vice-versa). Naming the stamp
# after the active mode means switching modes references an absent stamp; the
# rule then drops the sibling stamp and touches the new one, whose fresh mtime
# forces every object to recompile. Mirrors the top-level config.mak dependency
# that guards the cross build against ASan toggles.
COV_STAMP := $(BUILD_DIR)/.coverage-$(COVERAGE)

$(COV_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR)/.coverage-*
	@touch $@

# Mirror the armv8m target defines so tcc.h parses identically to the
# compiler build. Unit tests never codegen, so these only drive the
# preprocessor.
UT_DEFINES := \
	-DTCC_TARGET_ARM \
	-DTCC_ARM_EABI \
	-DTCC_ARM_VFP \
	-DTCC_ARM_HARDFLOAT \
	-DTCC_TARGET_ARM_THUMB \
	-DTCC_TARGET_ARM_ARCHV8M

# -ffunction-sections/-fdata-sections + -Wl,--gc-sections (link rule below) let
# us pull the irop_config[] table (ir/gen/config.c) out of the heavy ir/gen/*.c
# modules without dragging in their frontend functions (sym_push, vtop, ...) —
# unreferenced sections are GC'd.
UT_CFLAGS := -std=c11 -g -O0 -Wall -Werror -Wno-unused-function \
			 -Wno-declaration-after-statement \
			 -ffunction-sections -fdata-sections \
			 -I$(TOP) -I$(TOP)/ir -I$(TOP)/ir/opt -I$(TOP)/source/memory/include -I$(TOP)/source/utils/include -I$(TOP)/source/backend/arch/arm -I$(TOP)/source/backend/arch/arm/thumb -I$(TOP)/source/opt/ssa -I$(TOP)/source/opt/ssa/include -I$(TOP)/source/opt/ssa/string -I$(TOP)/source/opt/ssa/scalar -I$(TOP)/source/opt/ssa/memory -I$(TOP)/source/opt/flat/include -I$(TOP)/source/opt/framework -I$(UT_ROOT) $(UT_DEFINES) $(UT_COV_CFLAGS)
UT_DEPFLAGS := -MMD -MP

# Modules under test (built from tinycc sources, host compilation).
UT_MODULE_SRCS := \
	$(TOP)/ir/pool.c \
	$(TOP)/ir/type.c \
	$(TOP)/ir/vreg.c \
	$(TOP)/ir/dump.c \
	$(TOP)/ir/stack.c \
	$(TOP)/ir/ssa.c \
	$(TOP)/source/memory/vector.c \
	$(TOP)/source/opt/ssa/scalar/gvn.c \
	$(TOP)/ir/opt/ssa_opt_loop.c \
	$(TOP)/svalue.c \
	$(TOP)/source/backend/arch/arm/arm.c \
	$(TOP)/source/backend/arch/arm/thumb/thumb.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_adr.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_bitfield.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_block.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_mrs.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_branch.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_cmp.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_extend.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_alu_reg.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_alu_imm.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_dsp.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_tbb.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_shift_reg.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_shift_imm.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_system.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_vfp.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_ldrd.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_ldaex.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_ldrex.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_mem_exclusive.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_mem_imm.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_mem_reg.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_mem_unpriv.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_mov.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_ldr_literal.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_mul.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_mvn.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_pld.c \
	$(TOP)/source/backend/arch/arm/thumb/thop_rev.c \
	$(TOP)/tccir_operand.c \
	$(addprefix $(TOP)/,$(IR_GEN_FILES)) \
	$(TOP)/ir/opt_utils.c \
	$(TOP)/ir/opt_neg_chain.c \
	$(TOP)/source/opt/flat/scalar/known_bits.c \
	$(TOP)/source/opt/flat/scalar/const_prop_tmp.c \
	$(TOP)/source/opt/flat/scalar/value_tracking.c \
	$(TOP)/source/opt/flat/scalar/const_var_prop.c \
	$(TOP)/source/opt/flat/memory/symaddr_cse.c \
	$(TOP)/source/opt/ssa/memory/diamond_store_fwd.c \
	$(TOP)/ir/opt_copyprop.c \
	$(TOP)/ir/opt_cmp_fuse.c \
	$(TOP)/ir/opt_constprop.c \
	$(TOP)/ir/opt_du.c \
	$(TOP)/ir/opt_constfold.c \
	$(TOP)/ir/licm.c \
	$(TOP)/ir/opt_jump_thread.c \
	$(TOP)/source/opt/flat/scalar/setif_or_taut.c \
	$(TOP)/ir/opt_dead_lea_store.c \
	$(TOP)/ir/opt_dce.c \
	$(TOP)/ir/opt_branch.c \
	$(TOP)/source/opt/flat/scalar/vrp.c \
	$(TOP)/ir/opt.c \
	$(TOP)/ir/opt_alias.c \
	$(TOP)/ir/cfg.c \
	$(TOP)/ir/opt_loop_utils.c \
	$(TOP)/ir/opt_reroll.c \
	$(TOP)/ir/opt_bitfield.c \
	$(TOP)/source/opt/flat/scalar/const_aggregate.c \
	$(TOP)/ir/opt_memory.c \
	$(TOP)/ir/opt_dead_vla.c \
	$(TOP)/ir/opt_xform.c \
	$(TOP)/ir/opt_engine.c \
	$(TOP)/source/opt/flat/scalar/branch.c \
	$(TOP)/ir/opt_promote.c \
	$(TOP)/source/opt/flat/cfg/if_convert.c \
	$(TOP)/source/opt/flat/scalar/bool.c \
	$(TOP)/ir/opt_gens_fusion.c \
	$(TOP)/ir/opt_fusion.c \
	$(TOP)/source/opt/flat/scalar/call_result.c \
	$(TOP)/source/opt/flat/scalar/self_arith.c \
	$(TOP)/source/opt/flat/scalar/narrow_store.c \
	$(TOP)/source/opt/flat/scalar/cmp_const_offset.c \
	$(TOP)/source/opt/flat/scalar/self_copy.c \
	$(TOP)/source/opt/flat/scalar/var_tmp_fwd.c \
	$(TOP)/source/opt/flat/fusion/indexed_chain.c \
	$(TOP)/source/opt/flat/fusion/pair_reorder.c \
	$(TOP)/source/opt/flat/fusion/disp.c \
	$(TOP)/ir/opt_pipeline.c \
		$(TOP)/ir/regalloc.c \
		$(TOP)/ir/codegen.c \
		$(TOP)/ir/machine_op.c \
		$(TOP)/tccls.c \
		$(TOP)/tccmachine.c \
		$(TOP)/source/backend/arch/arm/arm_regalloc.c \
		$(TOP)/source/backend/arch/arm/arm_aapcs.c \
		$(TOP)/source/backend/arch/arm/ssa_opt_arm.c \
		$(TOP)/ir/opt_pack64.c \
		$(TOP)/ir/opt_loop_const_sim.c \
		$(TOP)/source/backend/arch/arm/thumb/arm-thumb-asm.c \
		$(TOP)/source/backend/arch/arm/arm-link.c \
		$(TOP)/tccld.c \
		$(TOP)/tccdebug.c \
		$(TOP)/tccyaff.c \
		$(TOP)/ir/opt_switch_data.c

# arm-thumb-asm.c stays in UT_MODULE_SRCS because the backend (UT2) and ssaopt
# (UT11) binaries link it for real (their test_gen_*/ssa suites need g()/
# gen_le16()/... ). But the MAIN binary's test_arm_thumb_asm.c #include's
# arm-thumb-asm.c directly (to reach its file-local `static` helpers), so its
# object must NOT also be linked into the main binary -- that would be a
# multiple-definition error for every ST_FUNC it exports. Filter it out of the
# main link only; UT2/UT11 re-pattern UT_MODULE_SRCS into their own build dirs
# and are unaffected.
UT_MODULE_OBJS := $(filter-out $(BUILD_DIR)/source/backend/arch/arm/thumb/arm-thumb-asm.o,\
	$(patsubst $(TOP)/%.c,$(BUILD_DIR)/%.o,$(UT_MODULE_SRCS)))

# Product sources that are never *linked* into the UT binary: each one's
# functions duplicate a name already provided by the stub layer below
# (stubs.c/codegen_mop_stubs.c/ra_link_stubs.c/tcc_state_stub.c exist
# specifically so tests don't have to drag in the frontend/ELF/linker/driver
# machinery these files depend on — see docs/plan_codegen_unit_tests.md §0
# for the arm-thumb-gen.c case). Linking them causes "multiple definition"
# errors. They're still compiled with -c only (never passed to the final
# link) purely so `make coverage` gets a .gcno for them and the report shows
# them (at 0%, since no unit test calls into them) instead of omitting them
# outright.
#
# ir/opt_switch_data.c moved OUT of this list (now in UT_MODULE_SRCS, linked
# for real) once elfsec_stubs.{c,h} supplied the missing ELF/section symbols
# (get_sym_ref/greloc/section_add/int_type) and the two pre-existing no-op
# stubs that duplicated its two entry points (tcc_ir_opt_switch_to_data in
# ra_link_stubs.c, tcc_ir_opt_switch_collapse_ex in stubs.c) were deleted.
UT_COVERAGE_ONLY_SRCS := \
	$(TOP)/source/backend/arch/arm/thumb/arm-thumb-gen.c \
	$(TOP)/source/backend/arch/arm/thumb/arm-thumb-callsite.c \
	$(TOP)/ir/opt/ssa_opt.c \
	$(TOP)/source/opt/ssa/cfg/branch.c \
	$(TOP)/source/opt/ssa/cfg/cmp_eq.c \
	$(TOP)/source/opt/ssa/scalar/cprop.c \
	$(TOP)/ir/opt/ssa_opt_dce.c \
	$(TOP)/ir/opt/ssa_opt_dead_loop.c \
	$(TOP)/source/opt/ssa/scalar/fold.c \
	$(TOP)/source/opt/ssa/memory/load_cse.c \
	$(TOP)/source/opt/ssa/scalar/narrow.c \
	$(TOP)/source/opt/ssa/cfg/phi.c \
	$(TOP)/source/opt/ssa/scalar/reassoc.c \
	$(TOP)/ir/opt/ssa_opt_sccp.c \
	$(TOP)/source/opt/ssa/scalar/strength.c \
	$(TOP)/tccgen.c \
	$(TOP)/tccdbg.c \
	$(TOP)/tccasm.c \
	$(TOP)/tccopt.c \
	$(TOP)/tcctools.c \
	$(TOP)/tcc.c \
	$(TOP)/libtcc.c
UT_COVERAGE_ONLY_OBJS := $(patsubst $(TOP)/%.c,$(BUILD_DIR)/%.o,$(UT_COVERAGE_ONLY_SRCS))

# Harness + suites (local to this directory).
UT_LOCAL_SRCS := \
	test_main.c \
	test_chained_hash.c \
	test_ir_pool.c \
	test_ir_type.c \
	test_ir_vreg.c \
	test_ir_core.c \
	test_ir_gen_control.c \
	test_ir_gen_vla.c \
	test_ir_dump.c \
	test_ir_stack.c \
	test_ir_ssa.c \
	test_ir_operand.c \
	test_svalue.c \
	test_tccls.c \
	test_ra_live.c \
	test_ra_linearscan.c \
	test_ra_phi.c \
	test_ra_arm.c \
	test_arm_target.c \
	test_arm_aapcs.c \
	test_arm_link.c \
	test_ld_script.c \
	test_opt_neg_chain.c \
	test_opt_knownbits.c \
	test_opt_symaddr_cse.c \
	test_opt_cmp_fuse.c \
	test_opt_helpers.c \
	test_opt_utils.c \
	test_opt_stack_addr_cse.c \
	test_opt_memset_fold.c \
	test_opt_dead_init_call.c \
	test_opt_memmove.c \
	test_opt_loop_utils.c \
	test_opt_du.c \
	test_opt_alias.c \
	test_ir_stack_extra.c \
	test_ir_stack_build.c \
	test_opt_cmpfold.c \
	test_opt_constprop.c \
	test_opt_constfold.c \
	test_opt_licm.c \
	test_opt_jump_thread.c \
	test_opt_setif_or_taut.c \
	test_opt_dead_lea_store.c \
	test_opt_loop_dead.c \
	test_opt_loop_const_sim.c \
	test_opt_pack64.c \
	test_opt_reroll.c \
	test_opt_bitfield.c \
	test_opt_const_aggregate.c \
	test_opt_memory.c \
	test_opt_memory_extra.c \
	test_opt_dce.c \
	test_opt_dead_store.c \
	test_opt_dce_cleanup.c \
	test_opt_store_fwd.c \
	test_opt_branch_cascade.c \
	test_opt_promote_extra.c \
	test_opt_fusion.c \
	test_opt_pipeline_orchestration.c \
	test_opt_vrp.c \
	test_opt_float_branch.c \
	test_opt_redundant_assign.c \
	test_opt_return_reuse.c \
	test_opt_uninit.c \
	test_opt_dead_vla.c \
	test_opt_xform.c \
	test_opt_switch_collapse.c \
	test_opt_switch_to_data.c \
	test_metamorphic.c \
	test_metamorphic_ssa.c \
	test_ssa_opt_arm.c \
	test_codegen_arith.c \
	test_codegen_mem.c \
	test_codegen_control.c \
	test_codegen_call.c \
	test_codegen_fp.c \
	test_codegen_atomic.c \
	test_codegen_dispatch_smoke.c \
	test_codegen_dispatch_prolog.c \
	test_tcc_driver.c \
	test_tccasm.c \
	test_tccdbg.c \
	test_tccdebug.c \
	test_thop_adr.c \
	test_thop_alu_reg.c \
	test_thop_alu_imm.c \
	test_thop_dsp.c \
	test_thop_bitfield.c \
	test_thop_block.c \
	test_thop_constraints.c \
	test_thumb_core.c \
	test_thop_branch.c \
	test_thop_mrs.c \
	test_thop_tbb.c \
	test_thop_shift_reg.c \
	test_thop_shift_imm.c \
	test_thop_system.c \
	test_thop_vfp.c \
	test_thop_cmp.c \
	test_thop_extend.c \
	test_thop_ldrd.c \
	test_thop_ldaex.c \
	test_thop_ldrex.c \
	test_thop_mem_exclusive.c \
	test_thop_mem_imm.c \
	test_thop_mem_reg.c \
	test_thop_mem_unpriv.c \
	test_thop_mov.c \
	test_thop_ldr_literal.c \
	test_thop_mul.c \
	test_thop_mvn.c \
	test_thop_pld.c \
	test_thop_rev.c \
	test_arm_thumb_asm.c \
	ra_link_stubs.c \
	elfsec_stubs.c \
	codegen_mop_stubs.c \
	stubs.c \
	stubs_gen_machine_fallback.c \
	tcc_state_stub.c
UT_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(UT_LOCAL_SRCS))

UT_OBJS := $(UT_LOCAL_OBJS) $(UT_MODULE_OBJS)
UT_DEPS := $(UT_OBJS:.o=.d)
UT_BIN := $(BUILD_DIR)/run_unit_tests

COV_DIR := $(BUILD_DIR)/coverage
# Report on every tinycc module compiled into the UT binary (all of
# UT_MODULE_SRCS: ir/, arch/arm/, and top-level product sources like
# tccgen.c/tccpp.c/arm-thumb-gen.c). The middle filter matches direct
# children of $(TOP) only (no further '/'), so it can't also match the local
# test-harness sources under tests/unit/arm/armv8m/ — those are excluded by
# simply not matching any filter below.
GCOVR_FILTERS := \
	--filter '$(TOP)/ir/' \
	--filter '$(TOP)/[^/]+\.c$$' \
	--filter '$(TOP)/source/backend/arch/arm/' \
	--filter '$(TOP)/source/opt/ssa/' \
	--filter '$(TOP)/source/memory/'

.PHONY: all run clean coverage run-tccpp coverage-tccpp run-tcc coverage-tcc

# The aggregate `all` and `run` targets are defined at the bottom of the file
# (after all UT*_BIN variables) so their prerequisite lists expand correctly.

# Clean instrumented build of EVERY unit-test binary (module, backend,
# tccgen, libtcc-api, tccopt, tccelf, tccpp, tcctools, tccyaff, tcc) → run each
# suite → render one merged gcov report.  gcovr is given all build trees at
# once, so a source file linked for real into more than one binary (e.g.
# arm-thumb-gen.c/arm-thumb-callsite.c in build_backend, tccgen.c in
# build_tccgen, libtcc.c in build_libtcc_api, tccelf.c in both build_tccelf
# and build_tccyaff) gets its per-binary hit counts summed into a single
# line/branch total instead of being reported multiple times.
# Outputs: build/coverage/index.html (browsable) + build/coverage/coverage.txt,
# plus a line/function/branch summary on the terminal. Requires gcovr.
#
# For fast iteration on a single binary instead of the full sweep, use
# `make coverage-backend` / `make coverage-libtcc-api` / `make coverage-tccopt`
# / `make coverage-tccelf` / `make coverage-tccpp` / `make coverage-tcctools`
# / `make coverage-tccyaff` / `make coverage-tcc`.
coverage:
	$(MAKE) clean
	$(MAKE) COVERAGE=1 $(UT_BIN)
	$(MAKE) COVERAGE=1 $(UT_COVERAGE_ONLY_OBJS)
	$(MAKE) COVERAGE=1 $(UT2_BIN)
	$(MAKE) COVERAGE=1 $(UT3_BIN)
	$(MAKE) COVERAGE=1 $(UT4_BIN)
	$(MAKE) COVERAGE=1 $(UT5_BIN)
	$(MAKE) COVERAGE=1 $(UT6_BIN)
	$(MAKE) COVERAGE=1 $(UT7_BIN)
	$(MAKE) COVERAGE=1 $(UT8_BIN)
	$(MAKE) COVERAGE=1 $(UT9_BIN)
	$(MAKE) COVERAGE=1 $(UT10_BIN)
	$(MAKE) COVERAGE=1 $(UT11_BIN)
	$(MAKE) COVERAGE=1 $(UT12_BIN)
	./$(UT_BIN)
	./$(UT2_BIN)
	./$(UT3_BIN)
	./$(UT4_BIN)
	./$(UT5_BIN)
	./$(UT6_BIN)
	./$(UT7_BIN)
	./$(UT8_BIN)
	./$(UT9_BIN)
	./$(UT10_BIN)
	./$(UT11_BIN)
	./$(UT12_BIN)
	@mkdir -p $(COV_DIR)
	gcovr --root $(TOP) \
		$(CURDIR)/$(BUILD_DIR) \
		$(CURDIR)/$(BUILD_DIR2) \
		$(CURDIR)/$(BUILD_DIR3) \
		$(CURDIR)/$(BUILD_DIR4) \
		$(CURDIR)/$(BUILD_DIR5) \
		$(CURDIR)/$(BUILD_DIR6) \
		$(CURDIR)/$(BUILD_DIR7) \
		$(CURDIR)/$(BUILD_DIR8) \
		$(CURDIR)/$(BUILD_DIR9) \
		$(CURDIR)/$(BUILD_DIR10) \
		$(CURDIR)/$(BUILD_DIR11) \
		$(CURDIR)/$(BUILD_DIR12) \
		$(GCOVR_FILTERS) \
		--exclude-unreachable-branches \
		--exclude-throw-branches \
		--sort uncovered-percent \
		--txt $(COV_DIR)/coverage.txt \
		--html-details $(COV_DIR)/index.html \
		--print-summary
	@echo ""
	@echo "Coverage report (module + backend + tccgen + libtcc-api + tccopt + tccelf + tccpp + tcctools + tccyaff + tcc, merged):"
	@echo "  text: $(CURDIR)/$(COV_DIR)/coverage.txt"
	@echo "  html: $(CURDIR)/$(COV_DIR)/index.html"

$(UT_BIN): $(UT_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections $(UT_COV_LDFLAGS) -o $@ $^

# Local source files → build/*.o
$(BUILD_DIR)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

# ir/pool.o etc — build from the tinycc source tree, preserve layout under build/.
$(BUILD_DIR)/ir/%.o: $(TOP)/ir/%.c Makefile $(COV_STAMP)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

# libtcc.o — build from the tinycc source tree.
$(BUILD_DIR)/libtcc.o: $(TOP)/libtcc.c Makefile $(COV_STAMP)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

# tccir_operand.o — top-level source (pool getters for the opt-pass suites).
$(BUILD_DIR)/tccir_operand.o: $(TOP)/tccir_operand.c Makefile $(COV_STAMP)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

# Other top-level tinycc sources (e.g. svalue.c) compiled straight to build/*.o.
$(BUILD_DIR)/%.o: $(TOP)/%.c Makefile $(COV_STAMP)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

# arch/arm/arm.o — build from the tinycc source tree.
$(BUILD_DIR)/arch/arm/%.o: $(TOP)/source/backend/arch/arm/%.c Makefile $(COV_STAMP)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

# arch/arm/thumb/thumb.o and thop_adr.o — build from the tinycc source tree.
$(BUILD_DIR)/arch/arm/thumb/%.o: $(TOP)/source/backend/arch/arm/thumb/%.c Makefile $(COV_STAMP)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

-include $(UT_DEPS)

# ---------------------------------------------------------------------------
# backend/ binary — links the REAL arm-thumb-gen.c + arm-thumb-callsite.c
# directly (bypassing ir/codegen.c's dispatch loop and codegen_mop_stubs.c),
# to call tcc_gen_machine_*_mop functions directly and assert on the real
# emitted Thumb-2 bytes. See docs/plan_codegen_unit_tests.md §0 for why this
# can't share the main run_unit_tests binary: codegen_mop_stubs.c fakes the
# exact ~78 tcc_gen_machine_* entry points arm-thumb-gen.c defines for real
# (guaranteed "multiple definition" on every one of them), so this is a
# second, independent binary with its own build tree. Part of the default
# `all`/`run`/`coverage` targets; `make run-backend` runs it alone.
# ---------------------------------------------------------------------------

BUILD_DIR2 := build_backend

COV_STAMP2 := $(BUILD_DIR2)/.coverage-$(COVERAGE)

$(COV_STAMP2):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR2)/.coverage-*
	@touch $@

# Reuses every module UT_MODULE_SRCS already links (ir/, arch/arm/, thop_*,
# arm-thumb-asm.c, arm-link.c, ir/opt_switch_data.c, ...) plus the two real
# backend files under test. Verified by trial-link: this exact source set
# links clean against UT2_LOCAL_SRCS below with zero undefined symbols and
# zero multiple definitions.
UT2_MODULE_SRCS := \
	$(UT_MODULE_SRCS) \
	$(TOP)/source/backend/arch/arm/thumb/arm-thumb-gen.c \
	$(TOP)/source/backend/arch/arm/thumb/arm-thumb-callsite.c
UT2_MODULE_OBJS := $(patsubst $(TOP)/%.c,$(BUILD_DIR2)/%.o,$(UT2_MODULE_SRCS))

# Harness + suites (local to this directory) for the backend binary. Reuses
# stubs.c/tcc_state_stub.c/ra_link_stubs.c/elfsec_stubs.c verbatim (same
# source, recompiled fresh into build_backend/ — safe, none of their symbols
# collide with the real arm-thumb-gen.c/arm-thumb-callsite.c). Does NOT link
# codegen_mop_stubs.c (fakes the real mop functions — multiple definition)
# or stubs_gen_machine_fallback.c (fakes two functions arm-thumb-gen.c
# provides for real here — multiple definition).
UT2_LOCAL_SRCS := \
	test_main2.c \
	test_gen_dispatch_smoke.c \
	test_gen_arith.c \
	test_gen_mem.c \
	test_gen_branch.c \
	test_gen_switch.c \
	test_gen_fp.c \
	test_gen_atomic.c \
	test_gen_call.c \
	test_gen_callsite.c \
	test_gen_prolog.c \
	test_gen_setjmp.c \
	test_gen_mach_operand.c \
	codegen_backend_stubs.c \
	elfsec_stubs.c \
	ra_link_stubs.c \
	stubs.c \
	tcc_state_stub.c
UT2_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR2)/%.o,$(UT2_LOCAL_SRCS))

UT2_OBJS := $(UT2_LOCAL_OBJS) $(UT2_MODULE_OBJS)
UT2_DEPS := $(UT2_OBJS:.o=.d)
UT2_BIN := $(BUILD_DIR2)/run_unit_tests_backend

COV_DIR2 := $(BUILD_DIR2)/coverage

.PHONY: run-backend coverage-backend
run-backend: $(UT2_BIN)
	./$(UT2_BIN)

coverage-backend:
	rm -rf $(BUILD_DIR2)
	$(MAKE) COVERAGE=1 $(UT2_BIN)
	./$(UT2_BIN)
	@mkdir -p $(COV_DIR2)
	gcovr --root $(TOP) $(CURDIR)/$(BUILD_DIR2) \
		$(GCOVR_FILTERS) \
		--exclude-unreachable-branches \
		--exclude-throw-branches \
		--sort uncovered-percent \
		--txt $(COV_DIR2)/coverage.txt \
		--html-details $(COV_DIR2)/index.html \
		--print-summary
	@echo ""
	@echo "Backend coverage report:"
	@echo "  text: $(CURDIR)/$(COV_DIR2)/coverage.txt"
	@echo "  html: $(CURDIR)/$(COV_DIR2)/index.html"

$(UT2_BIN): $(UT2_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections $(UT_COV_LDFLAGS) -o $@ $^

$(BUILD_DIR2)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP2)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR2)/ir/%.o: $(TOP)/ir/%.c Makefile $(COV_STAMP2)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR2)/arch/arm/thumb/%.o: $(TOP)/source/backend/arch/arm/thumb/%.c Makefile $(COV_STAMP2)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR2)/arch/arm/%.o: $(TOP)/source/backend/arch/arm/%.c Makefile $(COV_STAMP2)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR2)/%.o: $(TOP)/%.c Makefile $(COV_STAMP2)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

-include $(UT2_DEPS)

# ---------------------------------------------------------------------------
# tccgen/ binary — links the REAL tccgen.c directly. The main unit binary
# keeps frontend stubs for type_size/is_float/sym_push2/etc.; this isolated
# binary avoids those duplicate symbols while still sharing the UT harness.
# ---------------------------------------------------------------------------

BUILD_DIR3 := build_tccgen

COV_STAMP3 := $(BUILD_DIR3)/.coverage-$(COVERAGE)

$(COV_STAMP3):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR3)/.coverage-*
	@touch $@

# tccgen.c is NOT linked as a separate object here: test_tccgen.c #includes it
# directly (the test_tccdbg.c/test_tccasm.c pattern) so the suite can exercise
# tccgen.c's file-local `static` helpers, not just its ST_FUNC surface. Linking
# tccgen.o too would be a multiple-definition error. Its coverage is attributed
# to the test_tccgen.o TU (gcov tracks the #included source file).
UT3_MODULE_SRCS :=
UT3_MODULE_OBJS :=

UT3_LOCAL_SRCS := \
	test_main3.c \
	test_tccgen.c
UT3_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR3)/%.o,$(UT3_LOCAL_SRCS))

UT3_OBJS := $(UT3_LOCAL_OBJS) $(UT3_MODULE_OBJS)
UT3_DEPS := $(UT3_OBJS:.o=.d)
UT3_BIN := $(BUILD_DIR3)/run_unit_tests_tccgen

.PHONY: run-tccgen
run-tccgen: $(UT3_BIN)
	./$(UT3_BIN)

$(UT3_BIN): $(UT3_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections $(UT_COV_LDFLAGS) -o $@ $^ -lm

$(BUILD_DIR3)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP3)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR3)/%.o: $(TOP)/%.c Makefile $(COV_STAMP3)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

-include $(UT3_DEPS)

# ---------------------------------------------------------------------------
# libtcc-api/ binary — links the REAL libtcc.c directly. Unlike every other
# binary in this Makefile, libtcc.c itself owns tcc_state/tcc_malloc-family/
# tcc_enter_state/_tcc_error(_noabort) for real, so this binary links NO
# other stub file from this directory (stubs.c/tcc_state_stub.c would be
# multiple-definition clashes) — just libtcc.c plus one small new stub file
# (libtcc_api_stubs.c) for its ~35 remaining frontend/pipeline/ELF
# dependencies (all no-ops except cstr_*, real verbatim-algorithm copies —
# see that file). Tests the "A-bucket" surface: option/path/symbol state
# manipulation that doesn't need a real preprocessor or ELF writer.
# ---------------------------------------------------------------------------

BUILD_DIR4 := build_libtcc_api

COV_STAMP4 := $(BUILD_DIR4)/.coverage-$(COVERAGE)

$(COV_STAMP4):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR4)/.coverage-*
	@touch $@

UT4_MODULE_SRCS := \
	$(TOP)/libtcc.c
UT4_MODULE_OBJS := $(patsubst $(TOP)/%.c,$(BUILD_DIR4)/%.o,$(UT4_MODULE_SRCS))

UT4_LOCAL_SRCS := \
	test_main4.c \
	test_libtcc_lifecycle.c \
	test_libtcc_paths.c \
	test_libtcc_symbols.c \
	test_libtcc_options_opt.c \
	test_libtcc_options_target.c \
	test_libtcc_options_linker.c \
	test_libtcc_output_files.c \
	libtcc_api_stubs.c
UT4_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR4)/%.o,$(UT4_LOCAL_SRCS))

UT4_OBJS := $(UT4_LOCAL_OBJS) $(UT4_MODULE_OBJS)
UT4_DEPS := $(UT4_OBJS:.o=.d)
UT4_BIN := $(BUILD_DIR4)/run_unit_tests_libtcc_api

COV_DIR4 := $(BUILD_DIR4)/coverage

.PHONY: run-libtcc-api coverage-libtcc-api
run-libtcc-api: $(UT4_BIN)
	./$(UT4_BIN)

coverage-libtcc-api:
	rm -rf $(BUILD_DIR4)
	$(MAKE) COVERAGE=1 $(UT4_BIN)
	./$(UT4_BIN)
	@mkdir -p $(COV_DIR4)
	gcovr --root $(TOP) $(CURDIR)/$(BUILD_DIR4) \
		$(GCOVR_FILTERS) \
		--exclude-unreachable-branches \
		--exclude-throw-branches \
		--sort uncovered-percent \
		--txt $(COV_DIR4)/coverage.txt \
		--html-details $(COV_DIR4)/index.html \
		--print-summary
	@echo ""
	@echo "libtcc-api coverage report:"
	@echo "  text: $(CURDIR)/$(COV_DIR4)/coverage.txt"
	@echo "  html: $(CURDIR)/$(COV_DIR4)/index.html"

$(UT4_BIN): $(UT4_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections $(UT_COV_LDFLAGS) -o $@ $^

$(BUILD_DIR4)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP4)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR4)/%.o: $(TOP)/%.c Makefile $(COV_STAMP4)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

-include $(UT4_DEPS)

# ---------------------------------------------------------------------------
# tccopt/ binary — links the REAL tccopt.c directly. tccopt.c's only external
# dependencies are tcc_malloc/_mallocz/_realloc/_free and the global
# `tcc_state` (via USING_GLOBALS, reading tcc_state->opt_fp_offset_cache) plus
# libc memcpy/memset/strcmp, so this binary needs only a tiny memory-stub
# layer (tccopt_stubs.c) plus tcc_state_stub.c reused verbatim (same pattern
# build_backend uses to reuse elfsec_stubs.c). A separate binary is required
# because two of tccopt.c's OWN functions are already faked by stub files the
# main and backend binaries need for unrelated reasons: tcc_opt_fp_mat_cache_
# free is stubbed in stubs.c (main binary), and _clear/_lookup/_record are
# stubbed in codegen_mop_stubs.c/codegen_backend_stubs.c -- linking the real
# tccopt.c into any of those would be an immediate multiple-definition error.
# Part of the default `all`/`run` targets; `make run-tccopt` runs it alone.
# ---------------------------------------------------------------------------

BUILD_DIR5 := build_tccopt

COV_STAMP5 := $(BUILD_DIR5)/.coverage-$(COVERAGE)

$(COV_STAMP5):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR5)/.coverage-*
	@touch $@

UT5_MODULE_SRCS := \
	$(TOP)/tccopt.c
UT5_MODULE_OBJS := $(patsubst $(TOP)/%.c,$(BUILD_DIR5)/%.o,$(UT5_MODULE_SRCS))

UT5_LOCAL_SRCS := \
	test_main5.c \
	test_tccopt.c \
	tccopt_stubs.c \
	tcc_state_stub.c
UT5_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR5)/%.o,$(UT5_LOCAL_SRCS))

UT5_OBJS := $(UT5_LOCAL_OBJS) $(UT5_MODULE_OBJS)
UT5_DEPS := $(UT5_OBJS:.o=.d)
UT5_BIN := $(BUILD_DIR5)/run_unit_tests_tccopt

COV_DIR5 := $(BUILD_DIR5)/coverage

.PHONY: run-tccopt coverage-tccopt
run-tccopt: $(UT5_BIN)
	./$(UT5_BIN)

coverage-tccopt:
	rm -rf $(BUILD_DIR5)
	$(MAKE) COVERAGE=1 $(UT5_BIN)
	./$(UT5_BIN)
	@mkdir -p $(COV_DIR5)
	gcovr --root $(TOP) $(CURDIR)/$(BUILD_DIR5) \
		$(GCOVR_FILTERS) \
		--exclude-unreachable-branches \
		--exclude-throw-branches \
		--sort uncovered-percent \
		--txt $(COV_DIR5)/coverage.txt \
		--html-details $(COV_DIR5)/index.html \
		--print-summary
	@echo ""
	@echo "tccopt coverage report:"
	@echo "  text: $(CURDIR)/$(COV_DIR5)/coverage.txt"
	@echo "  html: $(CURDIR)/$(COV_DIR5)/index.html"

$(UT5_BIN): $(UT5_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections $(UT_COV_LDFLAGS) -o $@ $^

$(BUILD_DIR5)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP5)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR5)/%.o: $(TOP)/%.c Makefile $(COV_STAMP5)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

-include $(UT5_DEPS)

# ---------------------------------------------------------------------------
# tccelf/ binary — links the REAL tccelf.c directly.  tccelf.c's helpers
# (section/symbol allocation, string tables, hash tables, symbol sorting)
# are tested in isolation; the remaining frontend/debug/eh-frame/YAFF/asm
# entry points are supplied by tccelf_stubs.c.  Included in the default
# `all`/`run` targets.
# ---------------------------------------------------------------------------

BUILD_DIR6 := build_tccelf

COV_STAMP6 := $(BUILD_DIR6)/.coverage-$(COVERAGE)

$(COV_STAMP6):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR6)/.coverage-*
	@touch $@

UT6_MODULE_SRCS := \
	$(TOP)/tccelf.c
UT6_MODULE_OBJS := $(patsubst $(TOP)/%.c,$(BUILD_DIR6)/%.o,$(UT6_MODULE_SRCS))

UT6_LOCAL_SRCS := \
	test_main6.c \
	test_tccelf.c \
	tccelf_stubs.c \
	tcc_state_stub.c
UT6_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR6)/%.o,$(UT6_LOCAL_SRCS))

UT6_OBJS := $(UT6_LOCAL_OBJS) $(UT6_MODULE_OBJS)
UT6_DEPS := $(UT6_OBJS:.o=.d)
UT6_BIN := $(BUILD_DIR6)/run_unit_tests_tccelf

COV_DIR6 := $(BUILD_DIR6)/coverage

.PHONY: run-tccelf coverage-tccelf
run-tccelf: $(UT6_BIN)
	./$(UT6_BIN)

coverage-tccelf:
	rm -rf $(BUILD_DIR6)
	$(MAKE) COVERAGE=1 $(UT6_BIN)
	./$(UT6_BIN)
	@mkdir -p $(COV_DIR6)
	gcovr --root $(TOP) $(CURDIR)/$(BUILD_DIR6) \
		$(GCOVR_FILTERS) \
		--exclude-unreachable-branches \
		--exclude-throw-branches \
		--sort uncovered-percent \
		--txt $(COV_DIR6)/coverage.txt \
		--html-details $(COV_DIR6)/index.html \
		--print-summary
	@echo ""
	@echo "tccelf coverage report:"
	@echo "  text: $(CURDIR)/$(COV_DIR6)/coverage.txt"
	@echo "  html: $(CURDIR)/$(COV_DIR6)/index.html"

$(UT6_BIN): $(UT6_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections $(UT_COV_LDFLAGS) -o $@ $^

$(BUILD_DIR6)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP6)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR6)/%.o: $(TOP)/%.c Makefile $(COV_STAMP6)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

-include $(UT6_DEPS)

# ---------------------------------------------------------------------------
# tccpp/ binary — links the REAL tccpp.c directly.  tccpp.c hosts the CString
# helpers, token interner, token-string buffer management, and pragma-pack
# replay logic.  Its remaining frontend dependencies (memory allocators,
# sym_push2 for define_push during tccpp_new, dynarray_reset for tccpp_delete,
# and the error/warning reporters) are supplied by tccpp_stubs.c +
# tcc_state_stub.c.  A separate binary is required because tccpp.c defines
# globals (tok_ident, table_ident, ...) and entry points already faked by
# stubs.c in the main/backend binaries; linking the real tccpp.c into those
# would be a multiple-definition error.  Included in the default `all`/`run`
# targets.
# ---------------------------------------------------------------------------

BUILD_DIR7 := build_tccpp

COV_STAMP7 := $(BUILD_DIR7)/.coverage-$(COVERAGE)

$(COV_STAMP7):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR7)/.coverage-*
	@touch $@

UT7_MODULE_SRCS := \
	$(TOP)/tccpp.c
UT7_MODULE_OBJS := $(patsubst $(TOP)/%.c,$(BUILD_DIR7)/%.o,$(UT7_MODULE_SRCS))

UT7_LOCAL_SRCS := \
	test_main7.c \
	test_tccpp.c \
	tccpp_stubs.c \
	tcc_state_stub.c
UT7_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR7)/%.o,$(UT7_LOCAL_SRCS))

UT7_OBJS := $(UT7_LOCAL_OBJS) $(UT7_MODULE_OBJS)
UT7_DEPS := $(UT7_OBJS:.o=.d)
UT7_BIN := $(BUILD_DIR7)/run_unit_tests_tccpp

COV_DIR7 := $(BUILD_DIR7)/coverage

.PHONY: run-tccpp coverage-tccpp
run-tccpp: $(UT7_BIN)
	./$(UT7_BIN)

coverage-tccpp:
	rm -rf $(BUILD_DIR7)
	$(MAKE) COVERAGE=1 $(UT7_BIN)
	./$(UT7_BIN)
	@mkdir -p $(COV_DIR7)
	gcovr --root $(TOP) $(CURDIR)/$(BUILD_DIR7) \
		$(GCOVR_FILTERS) \
		--exclude-unreachable-branches \
		--exclude-throw-branches \
		--sort uncovered-percent \
		--txt $(COV_DIR7)/coverage.txt \
		--html-details $(COV_DIR7)/index.html \
		--print-summary
	@echo ""
	@echo "tccpp coverage report:"
	@echo "  text: $(CURDIR)/$(COV_DIR7)/coverage.txt"
	@echo "  html: $(CURDIR)/$(COV_DIR7)/index.html"

$(UT7_BIN): $(UT7_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections $(UT_COV_LDFLAGS) -o $@ $^

$(BUILD_DIR7)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP7)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR7)/%.o: $(TOP)/%.c Makefile $(COV_STAMP7)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

-include $(UT7_DEPS)

# ---------------------------------------------------------------------------
# tcctools/ binary — links the REAL tcctools.c directly.  tcctools.c's only
# external dependencies are the tcc_malloc-family allocators, tcc_fileextension,
# _tcc_error_noabort/tcc_enter_state, and the global `tcc_state`, all supplied
# by tcctools_stubs.c + tcc_state_stub.c.  A separate binary is required
# because tcctools.c's read16le/write16le/etc helpers are already defined by
# stubs.c/codegen_mop_stubs.c for the main/backend binaries; linking the real
# tcctools.c into either of those would be an immediate multiple-definition
# error.  Included in the default `all`/`run` targets per project request.
# ---------------------------------------------------------------------------

BUILD_DIR8 := build_tcctools

COV_STAMP8 := $(BUILD_DIR8)/.coverage-$(COVERAGE)

$(COV_STAMP8):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR8)/.coverage-*
	@touch $@

UT8_MODULE_SRCS := \
	$(TOP)/tcctools.c
UT8_MODULE_OBJS := $(patsubst $(TOP)/%.c,$(BUILD_DIR8)/%.o,$(UT8_MODULE_SRCS))

UT8_LOCAL_SRCS := \
	test_main8.c \
	test_tcctools.c \
	tcctools_stubs.c \
	tcc_state_stub.c
UT8_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR8)/%.o,$(UT8_LOCAL_SRCS))

UT8_OBJS := $(UT8_LOCAL_OBJS) $(UT8_MODULE_OBJS)
UT8_DEPS := $(UT8_OBJS:.o=.d)
UT8_BIN := $(BUILD_DIR8)/run_unit_tests_tcctools

COV_DIR8 := $(BUILD_DIR8)/coverage

.PHONY: run-tcctools coverage-tcctools run-tccctools coverage-tccctools
run-tcctools: $(UT8_BIN)
	./$(UT8_BIN)

coverage-tcctools:
	rm -rf $(BUILD_DIR8)
	$(MAKE) COVERAGE=1 $(UT8_BIN)
	./$(UT8_BIN)
	@mkdir -p $(COV_DIR8)
	gcovr --root $(TOP) $(CURDIR)/$(BUILD_DIR8) \
		$(GCOVR_FILTERS) \
		--exclude-unreachable-branches \
		--exclude-throw-branches \
		--sort uncovered-percent \
		--txt $(COV_DIR8)/coverage.txt \
		--html-details $(COV_DIR8)/index.html \
		--print-summary
	@echo ""
	@echo "tcctools coverage report:"
	@echo "  text: $(CURDIR)/$(COV_DIR8)/coverage.txt"
	@echo "  html: $(CURDIR)/$(COV_DIR8)/index.html"

# Compatibility aliases for the typo-prone target name requested by the task.
run-tccctools: run-tcctools
coverage-tccctools: coverage-tcctools

$(UT8_BIN): $(UT8_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections $(UT_COV_LDFLAGS) -o $@ $^

$(BUILD_DIR8)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP8)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR8)/%.o: $(TOP)/%.c Makefile $(COV_STAMP8)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

-include $(UT8_DEPS)

# ---------------------------------------------------------------------------
# tccyaff/ binary — links the REAL tccyaff.c + tccelf.c directly.  tccyaff.c
# needs the ELF symbol helpers (set_elf_sym/get_sym_attr/put_elf_reloc) and
# the global section pointers that live in tccelf.c; linking the real file is
# simpler than reimplementing those helpers.  The remaining frontend symbols
# (error handlers, tcc_basename, full_read, tcc_add_dllref) are supplied by
# tccyaff_stubs.c.  Included in the default `all`/`run` targets.
# ---------------------------------------------------------------------------

BUILD_DIR9 := build_tccyaff

COV_STAMP9 := $(BUILD_DIR9)/.coverage-$(COVERAGE)

$(COV_STAMP9):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR9)/.coverage-*
	@touch $@

UT9_MODULE_SRCS := \
	$(TOP)/tccyaff.c \
	$(TOP)/tccelf.c
UT9_MODULE_OBJS := $(patsubst $(TOP)/%.c,$(BUILD_DIR9)/%.o,$(UT9_MODULE_SRCS))

UT9_LOCAL_SRCS := \
	test_main9.c \
	test_tccyaff.c \
	tccyaff_stubs.c \
	tcc_state_stub.c
UT9_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR9)/%.o,$(UT9_LOCAL_SRCS))

UT9_OBJS := $(UT9_LOCAL_OBJS) $(UT9_MODULE_OBJS)
UT9_DEPS := $(UT9_OBJS:.o=.d)
UT9_BIN := $(BUILD_DIR9)/run_unit_tests_tccyaff

COV_DIR9 := $(BUILD_DIR9)/coverage

.PHONY: run-tccyaff coverage-tccyaff
run-tccyaff: $(UT9_BIN)
	./$(UT9_BIN)

coverage-tccyaff:
	rm -rf $(BUILD_DIR9)
	$(MAKE) COVERAGE=1 $(UT9_BIN)
	./$(UT9_BIN)
	@mkdir -p $(COV_DIR9)
	gcovr --root $(TOP) $(CURDIR)/$(BUILD_DIR9) \
		$(GCOVR_FILTERS) \
		--exclude-unreachable-branches \
		--exclude-throw-branches \
		--sort uncovered-percent \
		--txt $(COV_DIR9)/coverage.txt \
		--html-details $(COV_DIR9)/index.html \
		--print-summary
	@echo ""
	@echo "tccyaff coverage report:"
	@echo "  text: $(CURDIR)/$(COV_DIR9)/coverage.txt"
	@echo "  html: $(CURDIR)/$(COV_DIR9)/index.html"

$(UT9_BIN): $(UT9_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections $(UT_COV_LDFLAGS) -o $@ $^

$(BUILD_DIR9)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP9)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR9)/%.o: $(TOP)/%.c Makefile $(COV_STAMP9)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

-include $(UT9_DEPS)

# ---------------------------------------------------------------------------
# tcc/ binary — pulls in the REAL tcc.c (which #includes tcctools.c) directly.
# Tests the isolated driver helpers (tcc_is_64bit_operand, default_outputfile)
# without invoking main().  The stub layer supplies only the allocator and
# timing/error symbols that survive --gc-sections from these helper tests.
# ---------------------------------------------------------------------------

BUILD_DIR10 := build_tcc

COV_STAMP10 := $(BUILD_DIR10)/.coverage-$(COVERAGE)

$(COV_STAMP10):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR10)/.coverage-*
	@touch $@

UT10_MODULE_SRCS :=
UT10_MODULE_OBJS :=

UT10_LOCAL_SRCS := \
	test_main10.c \
	test_tcc.c \
	tcc_stubs.c
UT10_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR10)/%.o,$(UT10_LOCAL_SRCS))

UT10_OBJS := $(UT10_LOCAL_OBJS)
UT10_DEPS := $(UT10_OBJS:.o=.d)
UT10_BIN := $(BUILD_DIR10)/run_unit_tests_tcc

COV_DIR10 := $(BUILD_DIR10)/coverage

.PHONY: run-tcc coverage-tcc
run-tcc: $(UT10_BIN)
	./$(UT10_BIN)

coverage-tcc:
	rm -rf $(BUILD_DIR10)
	$(MAKE) COVERAGE=1 $(UT10_BIN)
	./$(UT10_BIN)
	@mkdir -p $(COV_DIR10)
	gcovr --root $(TOP) $(CURDIR)/$(BUILD_DIR10) \
		$(GCOVR_FILTERS) \
		--exclude-unreachable-branches \
		--exclude-throw-branches \
		--sort uncovered-percent \
		--txt $(COV_DIR10)/coverage.txt \
		--html-details $(COV_DIR10)/index.html \
		--print-summary
	@echo ""
	@echo "tcc coverage report:"
	@echo "  text: $(CURDIR)/$(COV_DIR10)/coverage.txt"
	@echo "  html: $(CURDIR)/$(COV_DIR10)/index.html"

$(UT10_BIN): $(UT10_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections $(UT_COV_LDFLAGS) -o $@ $^

$(BUILD_DIR10)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP10)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR10)/%.o: $(TOP)/%.c Makefile $(COV_STAMP10)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

-include $(UT10_DEPS)

# ---------------------------------------------------------------------------
# UT11 — SSA optimizer family. Links the REAL ir/opt/ssa_opt*.c on top of the
# shared IR module set, so the passes run against real SSA/CFG/vinfo built by
# ir/ssa.c + ir/cfg.c (already in UT_MODULE_SRCS).
# ---------------------------------------------------------------------------

BUILD_DIR11 := build_ssaopt

COV_STAMP11 := $(BUILD_DIR11)/.coverage-$(COVERAGE)

$(COV_STAMP11):
	@mkdir -p $(dir $@)
	@touch $@

UT11_MODULE_SRCS := \
	$(UT_MODULE_SRCS) \
	$(TOP)/libtcc.c \
	$(TOP)/ir/opt/ssa_opt.c \
	$(TOP)/source/opt/ssa/scalar/cprop.c \
	$(TOP)/ir/opt/ssa_opt_dce.c \
	$(TOP)/source/opt/ssa/scalar/fold.c \
	$(TOP)/ir/opt/ssa_opt_sccp.c \
	$(TOP)/source/opt/ssa/memory/load_cse.c \
	$(TOP)/source/opt/ssa/cfg/branch.c \
	$(TOP)/source/opt/ssa/cfg/cmp_eq.c \
	$(TOP)/source/opt/ssa/scalar/reassoc.c \
	$(TOP)/source/opt/ssa/scalar/narrow.c \
	$(TOP)/source/opt/ssa/cfg/phi.c \
	$(TOP)/source/opt/ssa/scalar/strength.c \
	$(TOP)/ir/opt/ssa_opt_dead_loop.c \
	$(TOP)/source/opt/ssa/string/const_string_fold.c \
	$(TOP)/source/opt/ssa/string/str_strlen.c \
	$(TOP)/source/opt/ssa/string/str_strcmp.c \
	$(TOP)/source/opt/ssa/string/str_strcpy.c \
	$(TOP)/source/opt/ssa/string/str_strspn.c \
	$(TOP)/source/opt/ssa/string/str_strchr.c \
	$(TOP)/source/opt/ssa/string/str_strstr.c \
	$(TOP)/source/opt/ssa/scalar/bitop_const_fold.c \
	$(TOP)/source/opt/ssa/scalar/var_imm_prop.c
	# gvn.c already in UT_MODULE_SRCS (source/opt/ssa/scalar/gvn.c)

UT11_MODULE_OBJS := $(patsubst $(TOP)/%.c,$(BUILD_DIR11)/%.o,$(UT11_MODULE_SRCS))

UT11_LOCAL_SRCS := \
	test_main11.c \
	test_ssa_opt_cprop.c \
	test_ssa_opt_sccp.c \
	test_ssa_opt_dead_loop.c \
	test_ssa_opt_dce_global.c \
	test_ssa_opt_gvn.c \
	test_ssa_opt_const_string_fold.c \
	test_ssa_opt_bitop_const_fold.c \
	test_ssa_opt_strength.c \
	test_ssa_opt_reassoc.c \
	test_ssa_opt_narrow.c \
	test_ssa_opt_fold.c \
	test_ssa_opt_phi.c \
	test_ssa_opt_branch.c \
	test_ssa_opt_cmp_eq.c \
	test_opt_ssa_domwalk.c \
	test_opt_dsl_pair.c \
	test_opt_dsl_phi.c \
	stubs.c \
	elfsec_stubs.c \
	tcc_state_stub.c
UT11_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR11)/%.o,$(UT11_LOCAL_SRCS))

UT11_OBJS := $(UT11_LOCAL_OBJS) $(UT11_MODULE_OBJS)
UT11_DEPS := $(UT11_OBJS:.o=.d)
UT11_BIN := $(BUILD_DIR11)/run_unit_tests_ssaopt

COV_DIR11 := $(BUILD_DIR11)/coverage

.PHONY: run-ssaopt coverage-ssaopt
run-ssaopt: $(UT11_BIN)
	./$(UT11_BIN)

coverage-ssaopt:
	rm -rf $(BUILD_DIR11)
	@mkdir -p $(UT11_DIRS)
	$(MAKE) COVERAGE=1 $(UT11_BIN)
	./$(UT11_BIN)
	@mkdir -p $(COV_DIR11)
	gcovr --root $(TOP) $(CURDIR)/$(BUILD_DIR11) \
		$(GCOVR_FILTERS) \
		--exclude-unreachable-branches \
		--exclude-throw-branches \
		--sort uncovered-percent \
		--txt $(COV_DIR11)/coverage.txt \
		--html-details $(COV_DIR11)/index.html \
		--print-summary
	@echo ""
	@echo "SSA optimizer coverage report:"
	@echo "  text: $(CURDIR)/$(COV_DIR11)/coverage.txt"
	@echo "  html: $(CURDIR)/$(COV_DIR11)/index.html"

$(UT11_BIN): $(UT11_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections $(UT_COV_LDFLAGS) -o $@ $^

# Local source files → build_ssaopt/*.o.  Compiled with -DUT_SSA_OPT_REAL so
# ra_link_stubs.c skips its ssa_opt no-ops (the real ssa_opt*.c provides them).
# Ensure all build_ssaopt subdirectories exist before any object is compiled.
# The per-rule mkdir -p races with parallel makes on the first build; an
# order-only directory stamp fixes it without rebuilding objects when the
# directory mtime changes.
UT11_DIRS := $(BUILD_DIR11)/ir/opt $(BUILD_DIR11)/arch/arm/thumb

$(UT11_DIRS):
	@mkdir -p $@

$(BUILD_DIR11)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP11) | $(UT11_DIRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) -DUT_SSA_OPT_REAL $(UT_DEPFLAGS) -c $< -o $@

# ir/opt/ssa_opt*.c — build from the tinycc source tree, preserve layout.
$(BUILD_DIR11)/ir/%.o: $(TOP)/ir/%.c Makefile $(COV_STAMP11) | $(UT11_DIRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) -DUT_SSA_OPT_REAL $(UT_DEPFLAGS) -c $< -o $@

# Other top-level tinycc sources compiled straight to build_ssaopt/*.o.
$(BUILD_DIR11)/%.o: $(TOP)/%.c Makefile $(COV_STAMP11) | $(UT11_DIRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) -DUT_SSA_OPT_REAL $(UT_DEPFLAGS) -c $< -o $@

-include $(UT11_DEPS)

# ---------------------------------------------------------------------------
# UT12 — memory utilities.
# ---------------------------------------------------------------------------

BUILD_DIR12 := build_unique_ptr

COV_STAMP12 := $(BUILD_DIR12)/.coverage-$(COVERAGE)

$(COV_STAMP12):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR12)/.coverage-*
	@touch $@

UT12_MODULE_SRCS := \
	$(TOP)/source/memory/unique_ptr.c \
	$(TOP)/source/memory/vector.c
UT12_MODULE_OBJS := $(patsubst $(TOP)/%.c,$(BUILD_DIR12)/%.o,$(UT12_MODULE_SRCS))

UT12_LOCAL_SRCS := \
	test_main12.c \
	test_defer.c \
	test_dynamic_bitset.c \
	test_bitspan.c \
	test_bit_matrix.c \
	test_small_sequence.c \
	test_unique_ptr.c \
	test_vector.c \
	stubs_unique_ptr.c
UT12_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR12)/%.o,$(UT12_LOCAL_SRCS))

UT12_OBJS := $(UT12_LOCAL_OBJS) $(UT12_MODULE_OBJS)
UT12_DEPS := $(UT12_OBJS:.o=.d)
UT12_BIN := $(BUILD_DIR12)/run_unit_tests_unique_ptr
UT12_CFLAGS := $(UT_CFLAGS)

COV_DIR12 := $(BUILD_DIR12)/coverage

.PHONY: run-unique-ptr coverage-unique-ptr
run-unique-ptr: $(UT12_BIN)
	./$(UT12_BIN)

coverage-unique-ptr:
	rm -rf $(BUILD_DIR12)
	$(MAKE) COVERAGE=1 $(UT12_BIN)
	./$(UT12_BIN)
	@mkdir -p $(COV_DIR12)
	gcovr --root $(TOP) $(CURDIR)/$(BUILD_DIR12) \
		$(GCOVR_FILTERS) \
		--exclude-unreachable-branches \
		--exclude-throw-branches \
		--sort uncovered-percent \
		--txt $(COV_DIR12)/coverage.txt \
		--html-details $(COV_DIR12)/index.html \
		--print-summary

$(UT12_BIN): $(UT12_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections -Wl,--wrap=tcc_free $(UT_COV_LDFLAGS) -o $@ $^

$(BUILD_DIR12)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP12)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT12_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

$(BUILD_DIR12)/%.o: $(TOP)/%.c Makefile $(COV_STAMP12)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT12_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

-include $(UT12_DEPS)

# ---------------------------------------------------------------------------
# Aggregate targets (defined here so all UT*_BIN variables are in scope).
# ---------------------------------------------------------------------------

all: $(UT_BIN) $(UT2_BIN) $(UT3_BIN) $(UT4_BIN) $(UT5_BIN) $(UT6_BIN) $(UT7_BIN) $(UT8_BIN) $(UT9_BIN) $(UT10_BIN) $(UT11_BIN) $(UT12_BIN)

run: $(UT_BIN) $(UT2_BIN) $(UT3_BIN) $(UT4_BIN) $(UT5_BIN) $(UT6_BIN) $(UT7_BIN) $(UT8_BIN) $(UT9_BIN) $(UT10_BIN) $(UT11_BIN) $(UT12_BIN)
	./$(UT_BIN)
	./$(UT2_BIN)
	./$(UT3_BIN)
	./$(UT4_BIN)
	./$(UT5_BIN)
	./$(UT6_BIN)
	./$(UT7_BIN)
	./$(UT8_BIN)
	./$(UT9_BIN)
	./$(UT10_BIN)
	./$(UT11_BIN)
	./$(UT12_BIN)

clean:
	rm -rf $(BUILD_DIR) $(BUILD_DIR2) $(BUILD_DIR3) $(BUILD_DIR4) $(BUILD_DIR5) $(BUILD_DIR6) $(BUILD_DIR7) $(BUILD_DIR8) $(BUILD_DIR9) $(BUILD_DIR10) $(BUILD_DIR11) $(BUILD_DIR12)
