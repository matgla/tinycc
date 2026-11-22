# Driver — build configuration
# Included by the top-level Makefile.
#
# The entry points: the CLI (tcc.c + the tool dispatch it textually includes)
# and the libtcc public API that drives option parsing, compilation and linking.

DRIVER_INC = -I$(TOP)/source/driver

# tcc.c is the CLI main().  It is linked into the executable directly rather
# than archived: no other TU references it, so as an archive member it would
# never be pulled in and the link would fail on a missing main().
DRIVER_MAIN_SRC = source/driver/tcc.c

# tcctools.c is #included by tcc.c, not compiled on its own — it stays out of
# DRIVER_SRC (and so out of the library) but is a dependency of tcc.o below.
DRIVER_SRC = \
	source/driver/libtcc.c

DRIVER_HDRS = \
	source/driver/libtcc.h

DRIVER_OBJ = $(call lib-objs,driver,$(DRIVER_SRC))
DRIVER_MAIN_OBJ = $(patsubst %.c,$(X)%.o,$(DRIVER_MAIN_SRC))
DRIVER_LIB = $(X)source/driver/libtccdriver.a

# tcchelpers.c and diag_out.c are #included by libtcc.c (tcchelpers.c also by the unit tests that compile
# tcc.c directly), never compiled on its own.
$(X)source/driver/libtcc.o: source/driver/tcchelpers.c source/driver/diag_out.c

$(X)source/driver/%.o: source/driver/%.c $(DRIVER_HDRS) $(TCC_CORE_DEPS)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

$(DRIVER_LIB): $(DRIVER_OBJ)
	$(ar-lib)
