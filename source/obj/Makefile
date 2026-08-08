# Object containers, linking and debug info — build configuration
# Included by the top-level Makefile.
#
# Everything that reads or writes an object/executable container: ELF sections
# and symbols, linker scripts, the YAFF flat format YasOS loads, and the
# DWARF/STABS debug-info emitters.

OBJ_INC = -I$(TOP)/source/obj

OBJ_SRC = \
	source/obj/tccelf.c \
	source/obj/tccld.c \
	source/obj/tccyaff.c \
	source/obj/tccdbg.c

OBJ_HDRS = \
	source/obj/dwarf.h \
	source/obj/elf.h \
	source/obj/stab.h \
	source/obj/tccld.h \
	source/obj/tccyaff.h

# stab.def is x-macro data included by stab.h, so it belongs in the dependency
# set even though it is not a .h file.
OBJ_HDRS += source/obj/stab.def

OBJ_OBJ = $(patsubst %.c,$(X)%.o,$(OBJ_SRC))
OBJ_LIB = $(X)source/obj/libobj.a

$(X)source/obj/%.o: source/obj/%.c $(OBJ_HDRS) $(TCC_CORE_DEPS)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

$(OBJ_LIB): $(OBJ_OBJ)
	$(ar-lib)
