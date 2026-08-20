# Optimization pipeline source files — build configuration
# Included by the top-level Makefile.

# Optimization pipeline compilation unit.
OPT_PIPELINE_INC = -I$(TOP)/source/opt

OPT_PIPELINE_CC = $(CC) $(CFLAGS) $(DEFINES) $(OPT_PIPELINE_INC)

# The pipeline TU depends on the full tcc header set and IR headers.
OPT_PIPELINE_OBJ = $(X)source/opt/function_pipeline.o

# The pipeline source is listed in the top-level Makefile's CORE_FILES.

$(OPT_PIPELINE_OBJ): \
	source/opt/function_pipeline.c \
	source/opt/function_pipeline.h \
	source/include/tcc.h source/ir/tccir.h source/opt/include/opt.h source/opt/include/opt_utils.h source/opt/include/opt_engine.h \
	source/opt/include/opt_pipeline.h source/opt/include/opt_gens_fusion.h source/opt/flat/include/opt/flat/bool.h \
	source/opt/flat/include/opt/flat/call_result.h \
	source/opt/flat/include/opt/flat/symaddr_cse.h source/ir/core.h \
	$(TCC_CORE_DEPS)
	@mkdir -p $(dir $@)
	$(OPT_PIPELINE_CC) -c $< -o $@

# Optimizer infrastructure relocated out of ir/opt*.c and tccopt.c: shared
# utilities, pre-SSA analyses, and the pass engine/pipeline driver.
CORE_OPT_SRC = \
	source/opt/util/pass_disable.c \
	source/opt/util/volatile_access.c \
	source/opt/util/const_eval.c \
	source/opt/util/const_string_eval.c \
	source/opt/util/cond_util.c \
	source/opt/util/block_scan.c \
	source/opt/util/purity.c \
	source/opt/util/expr_equal.c \
	source/opt/util/call_params.c \
	source/opt/util/vreg_query.c \
	source/opt/util/vreg_def_use.c \
	source/opt/util/xform.c \
	source/opt/util/ir_insert.c \
	source/opt/analysis/du_chains.c \
	source/opt/analysis/alias.c \
	source/opt/analysis/memref.c \
	source/opt/analysis/mem_ssa.c \
	source/opt/engine/ctx.c \
	source/opt/engine/run_gens.c \
	source/opt/engine/pipeline_run.c \
	source/opt/engine/pipeline_table.c \
	source/opt/engine/gen_adapters.c \
	source/opt/engine/pass_timing.c \
	source/opt/engine/fp_cache_shim.c \
	source/opt/engine/fp_mat_cache.c \
	source/opt/engine/pass_registry.c

# Interface headers these units depend on.  $(TCC_CORE_DEPS) is not optional:
# these TUs include source/include/tcc.h, so configure's defines decide struct TCCState's
# layout for them too.
CORE_OPT_HDRS = \
	source/include/tcc.h source/ir/tccir.h source/ir/ir.h $(wildcard source/opt/include/*.h) $(TCC_CORE_DEPS)

# Pre-RA cleanup passes, relocated out of ir/regalloc.c.
RA_OPT_SRC = \
	source/opt/ra/const_branch_fold.c \
	source/opt/ra/incomplete_calls.c \
	source/opt/ra/phi_const_chain.c

$(X)source/opt/ra/%.o: source/opt/ra/%.c $(CORE_OPT_HDRS) source/ir/regalloc.h
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

$(X)source/opt/util/%.o: source/opt/util/%.c $(CORE_OPT_HDRS)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

$(X)source/opt/analysis/%.o: source/opt/analysis/%.c $(CORE_OPT_HDRS)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

$(X)source/opt/engine/%.o: source/opt/engine/%.c $(CORE_OPT_HDRS)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

# The module library.  $(SSA_OPT_SRC) and $(FLAT_OPT_SRC) come from the ssa/ and
# flat/ sub-Makefiles, so this file must be included after both — prerequisite
# lists are expanded when the rule is read, not when it runs.
OPT_SRC = \
	source/opt/function_pipeline.c \
	$(CORE_OPT_SRC) \
	$(RA_OPT_SRC) \
	$(SSA_OPT_SRC) \
	$(FLAT_OPT_SRC)

OPT_OBJ = $(patsubst %.c,$(X)%.o,$(OPT_SRC))
OPT_LIB = $(X)source/opt/libopt.a

$(OPT_LIB): $(OPT_OBJ)
	$(ar-lib)

# Phony target for development workflow.
.PHONY: opt-pipeline-check

# Compile-check the pipeline TU; fails the build on any error.
opt-pipeline-check: $(OPT_PIPELINE_OBJ)
	@echo "Opt pipeline compiled OK."
