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
	-I$(TOP)/source/opt/ssa/cfg

# Compilation units — appended to IR_FILES in the top-level Makefile.
SSA_OPT_SRC = \
	source/opt/ssa/string/const_string_fold.c \
	source/opt/ssa/string/str_strlen.c \
	source/opt/ssa/string/str_strcmp.c \
	source/opt/ssa/string/str_strcpy.c \
	source/opt/ssa/string/str_strspn.c \
	source/opt/ssa/string/str_strchr.c \
	source/opt/ssa/string/str_strstr.c \
	source/opt/ssa/memory/global_addr_hoist.c \
	source/opt/ssa/scalar/bitop_const_fold.c \
	source/opt/ssa/scalar/strength.c \
	source/opt/ssa/scalar/narrow.c \
	source/opt/ssa/scalar/fold.c \
	source/opt/ssa/cfg/phi.c \
	source/opt/ssa/cfg/branch.c

# Header set the units depend on (force a rebuild on interface edits).
SSA_OPT_HDRS = \
	source/opt/ssa/string/str_handlers.h \
	source/opt/ssa/string/const_string_fold.h \
	source/opt/ssa/memory/global_addr_hoist.h \
	source/opt/ssa/scalar/bitop_const_fold.h \
	source/opt/ssa/include/opt/ssa/strength.h \
	source/opt/ssa/include/opt/ssa/narrow.h \
	source/opt/ssa/include/opt/ssa/fold.h \
	source/opt/ssa/include/opt/ssa/phi.h \
	source/opt/ssa/include/opt/ssa/branch.h \
	source/opt/ssa/include/opt/ssa/ssa_opt_helpers.h \
	$(OPT_DSL_HEADERS) \
	tcc.h tccir.h ir/ir.h ir/opt/ssa_opt.h ir/opt_utils.h

$(X)source/opt/ssa/%.o: source/opt/ssa/%.c $(SSA_OPT_HDRS) config.mak
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))
