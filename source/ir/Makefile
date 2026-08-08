# IR — build configuration
# Included by the top-level Makefile.
#
# The target-independent intermediate representation: the instruction pool and
# its operands, the CFG/SSA construction, the two-pass codegen dispatch loop and
# the SSA register allocator.  The gen/ subtree (source/ir/gen/Makefile) is the
# instruction-emission layer; its sources are folded into this module's library.

IR_INC = -I$(TOP)/source/ir

IR_SRC = \
	source/ir/type.c \
	source/ir/pool.c \
	source/ir/vreg.c \
	source/ir/stack.c \
	source/ir/dump.c \
	source/ir/codegen.c \
	source/ir/cfg.c \
	source/ir/ssa.c \
	source/ir/regalloc.c \
	source/ir/machine_op.c \
	source/ir/tccir_operand.c

IR_HDRS = $(wildcard source/ir/*.h)

# $(IR_GEN_FILES) comes from source/ir/gen/Makefile, included first by the
# top-level Makefile.
IR_OBJ = $(patsubst %.c,$(X)%.o,$(IR_SRC) $(IR_GEN_FILES))
IR_LIB = $(X)source/ir/libir.a

$(X)source/ir/%.o: source/ir/%.c $(IR_HDRS) $(TCC_CORE_DEPS)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

$(IR_LIB): $(IR_OBJ)
	$(ar-lib)
