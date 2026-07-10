# Optimization DSL Framework — Build Configuration
# Included by the top-level Makefile.

# Framework include path (relative to project root).
OPT_DSL_INC = -I$(TOP)/source/opt/framework

# DSL headers (header-only, no compilation unit).
OPT_DSL_HEADERS = \
	source/opt/framework/opt_dsl.h \
	source/opt/framework/opt_dsl_types.h \
	source/opt/framework/opt_dsl_helpers.h \
	source/opt/framework/opt_dsl_entry.h \
	source/opt/framework/opt_dsl_ssa.h \
	source/opt/framework/opt_dsl_phi.h

# Optional: example generators compiled as a test unit.
# Enable with: make cross OPT_DSL_EXAMPLES=yes
OPT_DSL_EXAMPLES ?= no

# The example TU is compile-checked (not linked): the generators reference IR
# inline accessors whose out-of-line symbols live in the core tcc objects, so a
# standalone link is not meaningful. VERIFY exercises the generator table too.
# $(DEF-$T) is empty at the top level (no CROSS_TARGET), so tcctok.h would
# default to i386 and pull i386-tok.h.  Force the armv8m target defines.
OPT_DSL_EXAMPLE_CC = $(CC) $(CFLAGS) $(DEFINES) $(DEF-armv8m) $(OPT_DSL_INC) -DOPT_DSL_EXAMPLE_VERIFY

ifeq ($(OPT_DSL_EXAMPLES),yes)
OPT_DSL_EXAMPLE_OBJECT = $(X)source/opt/framework/example_strength.o

$(OPT_DSL_EXAMPLE_OBJECT): \
	source/opt/framework/example_strength.c \
	$(OPT_DSL_HEADERS)
	@mkdir -p $(dir $@)
	$(OPT_DSL_EXAMPLE_CC) -c $< -o $@

.PHONY: opt-dsl-examples
opt-dsl-examples: $(OPT_DSL_EXAMPLE_OBJECT)
endif

# Phony targets for DSL development workflow.
.PHONY: opt-dsl-check opt-dsl-expand

# Compile the example generator TU; fails the build on any DSL error.
opt-dsl-check: $(OPT_DSL_HEADERS)
	@echo "Compiling DSL example under $(CC)..."
	$(OPT_DSL_EXAMPLE_CC) -fsyntax-only source/opt/framework/example_strength.c
	@echo "OK."

# Expand the example TU with gcc -E to inspect macro hygiene.
opt-dsl-expand:
	@echo "Expanding DSL example with $(CC) -E..."
	$(OPT_DSL_EXAMPLE_CC) -E source/opt/framework/example_strength.c 2>&1 | tail -80
	@echo "Done."
