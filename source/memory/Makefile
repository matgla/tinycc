# Memory utilities - build configuration
# Included by the top-level Makefile.

MEMORY_INC = -I$(TOP)/source/memory/include

MEMORY_SRC = \
	source/memory/unique_ptr.c \
	source/memory/vector.c

MEMORY_HDRS = \
	source/memory/include/memory/dynamic_bitset.h \
	source/memory/include/memory/small_sequence.h \
	source/memory/include/memory/unique_ptr.h \
	source/memory/include/memory/vector.h

# These TUs include tcc.h as well as their own headers, so they need the core
# header set and the build configuration behind it.
$(X)source/memory/%.o: source/memory/%.c $(MEMORY_HDRS) $(TCC_CORE_DEPS)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

MEMORY_OBJ = $(patsubst %.c,$(X)%.o,$(MEMORY_SRC))
MEMORY_LIB = $(X)source/memory/libmemory.a

$(MEMORY_LIB): $(MEMORY_OBJ)
	$(ar-lib)
