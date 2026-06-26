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

# Host compiler (do NOT use the armv8m cross compiler).
HOSTCC ?= gcc

# Build directory — all objects and the final binary go here.
BUILD_DIR := build

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
# us pull the irop_config[] table out of the heavy ir/core.c without dragging in
# its frontend functions (sym_push, vtop, ...) — unreferenced sections are GC'd.
UT_CFLAGS := -std=c11 -g -O0 -Wall -Werror -Wno-unused-function \
			 -Wno-declaration-after-statement \
			 -ffunction-sections -fdata-sections \
			 -I$(TOP) -I$(TOP)/ir -I$(UT_ROOT) $(UT_DEFINES)
UT_DEPFLAGS := -MMD -MP

# Modules under test (built from tinycc sources, host compilation).
UT_MODULE_SRCS := \
	$(TOP)/ir/pool.c \
	$(TOP)/ir/type.c \
	$(TOP)/ir/vreg.c \
	$(TOP)/arch/arm/arm.c \
	$(TOP)/arch/arm/thumb/thumb.c \
	$(TOP)/arch/arm/thumb/thop_adr.c \
	$(TOP)/arch/arm/thumb/thop_bitfield.c \
	$(TOP)/arch/arm/thumb/thop_block.c \
	$(TOP)/arch/arm/thumb/thop_mrs.c \
	$(TOP)/arch/arm/thumb/thop_branch.c \
	$(TOP)/arch/arm/thumb/thop_cmp.c \
	$(TOP)/arch/arm/thumb/thop_extend.c \
	$(TOP)/arch/arm/thumb/thop_alu_reg.c \
	$(TOP)/arch/arm/thumb/thop_tbb.c \
	$(TOP)/arch/arm/thumb/thop_shift_reg.c \
	$(TOP)/arch/arm/thumb/thop_shift_imm.c \
	$(TOP)/arch/arm/thumb/thop_system.c \
	$(TOP)/arch/arm/thumb/thop_vfp.c \
	$(TOP)/arch/arm/thumb/thop_ldrd.c \
	$(TOP)/arch/arm/thumb/thop_ldaex.c \
	$(TOP)/arch/arm/thumb/thop_ldrex.c \
	$(TOP)/arch/arm/thumb/thop_mem_exclusive.c \
	$(TOP)/arch/arm/thumb/thop_mem_imm.c \
	$(TOP)/arch/arm/thumb/thop_mem_reg.c \
	$(TOP)/arch/arm/thumb/thop_mem_unpriv.c \
	$(TOP)/arch/arm/thumb/thop_mov.c \
	$(TOP)/arch/arm/thumb/thop_ldr_literal.c \
	$(TOP)/arch/arm/thumb/thop_mul.c \
	$(TOP)/arch/arm/thumb/thop_mvn.c \
	$(TOP)/arch/arm/thumb/thop_pld.c \
	$(TOP)/arch/arm/thumb/thop_rev.c \
	$(TOP)/tccir_operand.c \
	$(TOP)/ir/core.c \
	$(TOP)/ir/opt_utils.c \
	$(TOP)/ir/opt_neg_chain.c \
	$(TOP)/ir/opt_knownbits.c \
	$(TOP)/ir/opt_copyprop.c \
	$(TOP)/ir/opt_cmp_fuse.c \
	$(TOP)/ir/opt_constprop.c \
	$(TOP)/ir/opt_du.c \
	$(TOP)/ir/opt_constfold.c \
	$(TOP)/ir/licm.c \
	$(TOP)/ir/opt_jump_thread.c \
	$(TOP)/ir/opt_setif_or_taut.c \
	$(TOP)/ir/opt_dead_lea_store.c \
	$(TOP)/ir/opt_dce.c \
	$(TOP)/ir/opt.c \
	$(TOP)/ir/opt_alias.c \
	$(TOP)/ir/cfg.c

UT_MODULE_OBJS := $(patsubst $(TOP)/%.c,$(BUILD_DIR)/%.o,$(UT_MODULE_SRCS))

# Harness + suites (local to this directory).
UT_LOCAL_SRCS := \
	test_thop_tbb.c \
	test_thop_shift_reg.c \
	test_thop_shift_imm.c \
	test_thop_system.c \
	test_thop_vfp.c \
	test_main.c \
	test_thop_mrs.c \
	test_thop_bitfield.c \
	test_chained_hash.c \
	test_ir_pool.c \
	test_ir_type.c \
	test_ir_vreg.c \
	test_opt_neg_chain.c \
	test_opt_knownbits.c \
	test_opt_copyprop.c \
	test_opt_cmp_fuse.c \
	test_opt_cmpfold.c \
	test_opt_constprop.c \
	test_opt_constfold.c \
	test_opt_licm.c \
	test_opt_jump_thread.c \
	test_opt_setif_or_taut.c \
	test_opt_dead_lea_store.c \
	test_thop_adr.c \
	test_thop_alu_reg.c \
	test_thop_block.c \
	test_thop_constraints.c \
	test_thop_branch.c \
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
	stubs.c \
	tcc_state_stub.c
UT_LOCAL_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(UT_LOCAL_SRCS))

UT_OBJS := $(UT_LOCAL_OBJS) $(UT_MODULE_OBJS)
UT_DEPS := $(UT_OBJS:.o=.d)
UT_BIN := $(BUILD_DIR)/run_unit_tests

.PHONY: all run clean
all: $(UT_BIN)

run: $(UT_BIN)
	./$(UT_BIN)

$(UT_BIN): $(UT_OBJS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -Wl,--gc-sections -o $@ $^

# Local source files → build/*.o
$(BUILD_DIR)/%.o: %.c $(UT_ROOT)/ut.h Makefile
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

# ir/pool.o etc — build from the tinycc source tree, preserve layout under build/.
$(BUILD_DIR)/ir/%.o: $(TOP)/ir/%.c Makefile
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

# libtcc.o — build from the tinycc source tree.
$(BUILD_DIR)/libtcc.o: $(TOP)/libtcc.c Makefile
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

# tccir_operand.o — top-level source (pool getters for the opt-pass suites).
$(BUILD_DIR)/tccir_operand.o: $(TOP)/tccir_operand.c Makefile
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

# arch/arm/arm.o — build from the tinycc source tree.
$(BUILD_DIR)/arch/arm/%.o: $(TOP)/arch/arm/%.c Makefile
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

# arch/arm/thumb/thumb.o and thop_adr.o — build from the tinycc source tree.
$(BUILD_DIR)/arch/arm/thumb/%.o: $(TOP)/arch/arm/thumb/%.c Makefile
	@mkdir -p $(dir $@)
	$(HOSTCC) $(UT_CFLAGS) $(UT_DEPFLAGS) -c $< -o $@

-include $(UT_DEPS)

clean:
	rm -rf $(BUILD_DIR)
