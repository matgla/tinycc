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
	tcc.h tccir.h ir/opt.h ir/opt_utils.h ir/opt_engine.h \
	ir/opt_pipeline.h ir/opt_gens_fusion.h ir/opt_gens_bool.h \
	ir/opt_gens_call_result.h ir/core.h
	@mkdir -p $(dir $@)
	$(OPT_PIPELINE_CC) -c $< -o $@

# Phony target for development workflow.
.PHONY: opt-pipeline-check

# Compile-check the pipeline TU; fails the build on any error.
opt-pipeline-check: $(OPT_PIPELINE_OBJ)
	@echo "Opt pipeline compiled OK."
