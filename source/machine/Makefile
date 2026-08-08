# Machine layer — build configuration
# Included by the top-level Makefile.
#
# The generic side of the backend boundary: the target/ABI description headers
# every arch backend implements against, and the linear-scan allocator that is
# parameterised by them.

MACHINE_INC = -I$(TOP)/source/machine

MACHINE_SRC = \
	source/machine/tccls.c \
	source/machine/tccmachine.c

MACHINE_HDRS = \
	source/machine/tccabi.h \
	source/machine/tccls.h \
	source/machine/tccmachine.h \
	source/machine/tcc_target.h

MACHINE_OBJ = $(patsubst %.c,$(X)%.o,$(MACHINE_SRC))
MACHINE_LIB = $(X)source/machine/libmachine.a

$(X)source/machine/%.o: source/machine/%.c $(MACHINE_HDRS) $(TCC_CORE_DEPS)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

$(MACHINE_LIB): $(MACHINE_OBJ)
	$(ar-lib)
