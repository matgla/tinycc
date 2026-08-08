# Support utilities — build configuration
# Included by the top-level Makefile.
#
# Cross-cutting helpers with no place in the compilation pipeline itself:
# logging, the chained hash table, the getenv-driven debug/bisect knobs, and
# the SValue/Sym pretty-printers used by -dump-ir and by gdb sessions.

SUPPORT_INC = -I$(TOP)/source/support

SUPPORT_SRC = \
	source/support/tccdebug.c

SUPPORT_HDRS = \
	source/support/log.h \
	source/support/tcc-chained-hash.h \
	source/support/tccdbgenv.h \
	source/support/tccdebug.h

SUPPORT_OBJ = $(patsubst %.c,$(X)%.o,$(SUPPORT_SRC))
SUPPORT_LIB = $(X)source/support/libsupport.a

$(X)source/support/%.o: source/support/%.c $(SUPPORT_HDRS) $(TCC_CORE_DEPS)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

$(SUPPORT_LIB): $(SUPPORT_OBJ)
	$(ar-lib)
