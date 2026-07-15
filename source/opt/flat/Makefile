# Flat (pre-SSA) optimization pass source files — build configuration
# Included by the top-level Makefile.  Landing zone for flat opt_gens_* passes
# relocated out of ir/ onto the source/opt/framework DSL.

# Include paths added to the global DEFINES by the top-level Makefile.
FLAT_OPT_INC = -I$(TOP)/source/opt/flat -I$(TOP)/source/opt/flat/include \
	-I$(TOP)/source/opt/flat/scalar -I$(TOP)/source/opt/flat/cfg \
	-I$(TOP)/source/opt/flat/fusion -I$(TOP)/source/opt/flat/memory

# Compilation units — appended to IR_FILES in the top-level Makefile.
FLAT_OPT_SRC = \
	source/opt/flat/scalar/bool.c \
	source/opt/flat/scalar/branch.c \
	source/opt/flat/scalar/call_result.c \
	source/opt/flat/scalar/self_arith.c \
	source/opt/flat/scalar/narrow_store.c \
	source/opt/flat/scalar/cmp_const_offset.c \
	source/opt/flat/scalar/self_copy.c \
	source/opt/flat/scalar/setif_or_taut.c \
	source/opt/flat/scalar/var_tmp_fwd.c \
	source/opt/flat/scalar/known_bits.c \
	source/opt/flat/scalar/const_prop_tmp.c \
	source/opt/flat/scalar/value_tracking.c \
	source/opt/flat/scalar/const_var_prop.c \
	source/opt/flat/scalar/const_aggregate.c \
	source/opt/flat/scalar/vrp.c \
	source/opt/flat/memory/symaddr_cse.c \
	source/opt/flat/fusion/indexed_chain.c \
	source/opt/flat/fusion/pair_reorder.c \
	source/opt/flat/fusion/disp.c \
	source/opt/flat/cfg/if_convert.c

# Header set the units depend on (force a rebuild on interface edits).
FLAT_OPT_HDRS = \
	source/opt/flat/include/opt/flat/bool.h \
	source/opt/flat/include/opt/flat/branch.h \
	source/opt/flat/include/opt/flat/call_result.h \
	source/opt/flat/include/opt/flat/self_arith.h \
	source/opt/flat/include/opt/flat/narrow_store.h \
	source/opt/flat/include/opt/flat/cmp_const_offset.h \
	source/opt/flat/include/opt/flat/self_copy.h \
	source/opt/flat/include/opt/flat/var_tmp_fwd.h \
	source/opt/flat/include/opt/flat/known_bits.h \
	source/opt/flat/include/opt/flat/symaddr_cse.h \
	source/opt/flat/include/opt/flat/indexed_chain.h \
	source/opt/flat/include/opt/flat/pair_reorder.h \
	source/opt/flat/include/opt/flat/disp.h \
	source/opt/flat/include/opt/flat/if_convert.h \
	$(OPT_DSL_HEADERS) \
	tcc.h tccir.h ir/ir.h ir/opt_engine.h

$(X)source/opt/flat/%.o: source/opt/flat/%.c $(FLAT_OPT_HDRS) config.mak
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))
