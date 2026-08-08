# Frontend C-parser / IR-emission sources — build configuration
# Included by the top-level Makefile.
#
# These are the split of the former monolithic tccgen.c (31,434 lines) into
# 52 files under 13 directories; see docs/plan_tccgen_split.md for the map
# and for why each cross-file symbol in gen_priv.h had to be promoted.

# Include paths added to the global DEFINES by the top-level Makefile.
# Only the gen/ root is needed: every TU includes "gen_priv.h" from there,
# and nothing includes a sibling directory's header directly.
GEN_INC = -I$(TOP)/source/frontend/gen

# Compilation units — appended to CORE_FILES in the top-level Makefile.
GEN_SRC = \
	source/frontend/gen/builtin/call.c \
	source/frontend/gen/builtin/chk.c \
	source/frontend/gen/builtin/fp.c \
	source/frontend/gen/builtin/fp2.c \
	source/frontend/gen/builtin/misc.c \
	source/frontend/gen/builtin/overflow.c \
	source/frontend/gen/builtin/simd.c \
	source/frontend/gen/builtin/string.c \
	source/frontend/gen/core/predicates.c \
	source/frontend/gen/core/state.c \
	source/frontend/gen/core/suppress.c \
	source/frontend/gen/decl/attribute.c \
	source/frontend/gen/decl/btype.c \
	source/frontend/gen/decl/decl.c \
	source/frontend/gen/decl/declarator.c \
	source/frontend/gen/decl/predef_protos.c \
	source/frontend/gen/decl/struct.c \
	source/frontend/gen/expr/atomic.c \
	source/frontend/gen/expr/cond.c \
	source/frontend/gen/expr/indir.c \
	source/frontend/gen/expr/infix.c \
	source/frontend/gen/expr/primary.c \
	source/frontend/gen/expr/unary.c \
	source/frontend/gen/init/alloc.c \
	source/frontend/gen/init/initializer.c \
	source/frontend/gen/inline/analysis.c \
	source/frontend/gen/inline/const_eval.c \
	source/frontend/gen/inline/emit.c \
	source/frontend/gen/nested/nested.c \
	source/frontend/gen/op/complex.c \
	source/frontend/gen/op/float.c \
	source/frontend/gen/op/fold_math.c \
	source/frontend/gen/op/int.c \
	source/frontend/gen/op/op.c \
	source/frontend/gen/op/vector.c \
	source/frontend/gen/stmt/block.c \
	source/frontend/gen/stmt/cleanup.c \
	source/frontend/gen/stmt/ret.c \
	source/frontend/gen/stmt/switch.c \
	source/frontend/gen/store/struct_copy.c \
	source/frontend/gen/store/vstore.c \
	source/frontend/gen/sym/attr_merge.c \
	source/frontend/gen/sym/elfsym.c \
	source/frontend/gen/sym/symtab.c \
	source/frontend/gen/type/assign_check.c \
	source/frontend/gen/type/cast.c \
	source/frontend/gen/type/compare.c \
	source/frontend/gen/type/size.c \
	source/frontend/gen/value/load.c \
	source/frontend/gen/value/longlong.c \
	source/frontend/gen/value/strlit_pool.c \
	source/frontend/gen/value/vstack.c

GEN_HDRS = \
	source/frontend/gen/gen_priv.h

# Each gen TU includes gen_priv.h, which pulls in tcc.h and the IR/opt headers,
# so they need the core header set and the build configuration behind it.
$(X)source/frontend/gen/%.o: source/frontend/gen/%.c $(GEN_HDRS) $(TCC_CORE_DEPS)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))
