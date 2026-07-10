# Backend code-generator source files — build configuration
# Included by the top-level Makefile.

# Backend generators compilation unit.
BACKEND_GENERATORS_INC = -I$(TOP)/source/backend/generators

BACKEND_GENERATORS_CC = $(CC) $(CFLAGS) $(DEFINES) $(BACKEND_GENERATORS_INC)

# Object files for the backend generator TUs.
BACKEND_GENERATORS_OBJ = $(X)source/backend/generators/function.o \
	$(X)source/backend/generators/regalloc.o

# The function generator depends on the full tcc header set and IR headers.
$(X)source/backend/generators/function.o: \
	source/backend/generators/function.c \
	source/backend/generators/regalloc.h \
	source/opt/function_pipeline.h \
	tcc.h tccir.h ir/opt.h ir/opt_utils.h ir/opt_engine.h \
	ir/opt_pipeline.h ir/opt_gens_fusion.h ir/opt_gens_bool.h \
	ir/opt_gens_call_result.h ir/regalloc.h ir/ssa.h \
	ir/cfg.h ir/codegen.h ir/core.h ir/licm.h \
	arch/arm/arm_regalloc.h
	@mkdir -p $(dir $@)
	$(BACKEND_GENERATORS_CC) -c $< -o $@

# The RA / codegen-prep pipeline (split from source/opt/function_pipeline.c).
$(X)source/backend/generators/regalloc.o: \
	source/backend/generators/regalloc.c \
	source/backend/generators/regalloc.h \
	tcc.h tccir.h ir/opt.h ir/opt_utils.h ir/opt_engine.h \
	ir/opt_pipeline.h ir/regalloc.h ir/ssa.h \
	ir/cfg.h ir/codegen.h ir/core.h ir/licm.h \
	arch/arm/arm_regalloc.h
	@mkdir -p $(dir $@)
	$(BACKEND_GENERATORS_CC) -c $< -o $@

# Phony target for development workflow.
.PHONY: backend-generators-check

# Compile-check the generator TU; fails the build on any error.
backend-generators-check: $(BACKEND_GENERATORS_OBJ)
	@echo "Backend generators compiled OK."
