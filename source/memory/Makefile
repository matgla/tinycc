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

$(X)source/memory/%.o: source/memory/%.c $(MEMORY_HDRS) config.mak
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))
