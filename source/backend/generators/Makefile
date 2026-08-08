# Backend code-generator source files — build configuration
# Included by the top-level Makefile.

# Backend generators compilation unit.
BACKEND_GENERATORS_INC = -I$(TOP)/source/backend/generators -I$(TOP)/source/backend/arch/arm

BACKEND_GENERATORS_CC = $(CC) $(CFLAGS) $(DEFINES) $(BACKEND_GENERATORS_INC)

# Object files for the backend generator TUs.
BACKEND_GENERATORS_SRC = source/backend/generators/function.c \
	source/backend/generators/regalloc.c

BACKEND_GENERATORS_OBJ = $(patsubst %.c,$(X)%.o,$(BACKEND_GENERATORS_SRC))

# The function generator depends on the full tcc header set and IR headers.
$(X)source/backend/generators/function.o: \
	source/backend/generators/function.c \
	source/backend/generators/regalloc.h \
	source/opt/function_pipeline.h \
	source/include/tcc.h source/ir/tccir.h source/opt/include/opt.h source/opt/include/opt_utils.h source/opt/include/opt_engine.h \
	source/opt/include/opt_pipeline.h source/opt/include/opt_gens_fusion.h source/opt/flat/include/opt/flat/bool.h \
	source/opt/flat/include/opt/flat/call_result.h source/ir/regalloc.h source/ir/ssa.h \
	source/ir/cfg.h source/ir/codegen.h source/ir/core.h source/opt/include/licm.h \
	source/backend/arch/arm/arm_regalloc.h \
	$(TCC_CORE_DEPS)
	@mkdir -p $(dir $@)
	$(BACKEND_GENERATORS_CC) -c $< -o $@

# The RA / codegen-prep pipeline (split from source/opt/function_pipeline.c).
$(X)source/backend/generators/regalloc.o: \
	source/backend/generators/regalloc.c \
	source/backend/generators/regalloc.h \
	source/include/tcc.h source/ir/tccir.h source/opt/include/opt.h source/opt/include/opt_utils.h source/opt/include/opt_engine.h \
	source/opt/include/opt_pipeline.h source/ir/regalloc.h source/ir/ssa.h \
	source/ir/cfg.h source/ir/codegen.h source/ir/core.h source/opt/include/licm.h \
	source/backend/arch/arm/arm_regalloc.h \
	$(TCC_CORE_DEPS)
	@mkdir -p $(dir $@)
	$(BACKEND_GENERATORS_CC) -c $< -o $@

BACKEND_GENERATORS_LIB = $(X)source/backend/generators/libgenerators.a

$(BACKEND_GENERATORS_LIB): $(BACKEND_GENERATORS_OBJ)
	$(ar-lib)

# Phony target for development workflow.
.PHONY: backend-generators-check

# Compile-check the generator TU; fails the build on any error.
backend-generators-check: $(BACKEND_GENERATORS_OBJ)
	@echo "Backend generators compiled OK."
