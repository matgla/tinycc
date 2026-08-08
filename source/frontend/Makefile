# Frontend — build configuration
# Included by the top-level Makefile.
#
# C source text in, IR out.  The preprocessor/tokenizer and the GAS-style asm
# frontend live here; the parser and IR-emission layer is the gen/ subtree
# (source/frontend/gen/Makefile), whose sources are folded into this module's
# library so the whole frontend ships as one archive.

FRONTEND_INC = -I$(TOP)/source/frontend

FRONTEND_SRC = \
	source/frontend/tccpp.c \
	source/frontend/tccasm.c \
	source/frontend/svalue.c

FRONTEND_HDRS = \
	source/frontend/svalue.h \
	source/frontend/tcctok.h \
	source/frontend/tcctype.h \
	source/frontend/tcc_scope.h

# $(GEN_SRC) comes from source/frontend/gen/Makefile, included first by the
# top-level Makefile.
FRONTEND_OBJ = $(patsubst %.c,$(X)%.o,$(FRONTEND_SRC) $(GEN_SRC))
FRONTEND_LIB = $(X)source/frontend/libfrontend.a

$(X)source/frontend/%.o: source/frontend/%.c $(FRONTEND_HDRS) $(TCC_CORE_DEPS)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

$(FRONTEND_LIB): $(FRONTEND_OBJ)
	$(ar-lib)
