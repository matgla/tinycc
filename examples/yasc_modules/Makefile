# yasc modules example: see README.md.
#
#   make                 build demo (yasc) and c_demo (plain C) with tcc
#   make modules         only the modules: objects, interfaces, C headers
#   make clean
#
# Without -ccompat tcc compiles yasc (safe by default), so the modules and
# main.c need no flag; c_main.c is plain C and says -ccompat.

TCC ?= tcc
CFLAGS ?= -O1
LDFLAGS ?=

MODULES = vec.c shapes.c
MODULE_OBJS = $(MODULES:.c=.o)
SRCS = $(MODULES) main.c c_main.c

all: demo c_demo

modules: $(MODULE_OBJS)

demo: main.o $(MODULE_OBJS)
	$(TCC) $(LDFLAGS) $^ -o $@

c_demo: c_main.o $(MODULE_OBJS)
	$(TCC) $(LDFLAGS) $^ -o $@

# A module's object comes with its interface (lib/vec.ymi, shapes.ymi, next
# to the object) and, with -fmodule-header=, its C header (include/...).
# -MD records every interface a compile read, so a changed interface
# rebuilds its importers -- and only a change: an unchanged one keeps its time.
%.o: %.c
	$(TCC) $(CFLAGS) -fmodule-header=include -c $< -o $@ -MD -MF $*.d

c_main.o: c_main.c
	$(TCC) $(CFLAGS) -ccompat -Iinclude -c $< -o $@ -MD -MF $*.d

# Build order from the sources themselves: tcc -fmodule-deps prints
# "module NAME" / "import NAME" without compiling anything.
#   module a.b  ->  a/b.ymi and include/a/b.h come with this object
#   import a.b  ->  this object needs a/b.ymi first
%.mdep: %.c
	$(TCC) -fmodule-deps $< | awk -v o=$*.o '{ i = $$2; gsub(/\./, "/", i) } \
	  $$1 == "module" { print i ".ymi include/" i ".h: " o " ;" } \
	  $$1 == "import" { print o ": " i ".ymi" }' > $@

# plain C has no imports to scan: it needs the headers
c_main.o: include/shapes.h include/lib/vec.h

include $(MODULES:.c=.mdep) main.mdep
-include $(SRCS:.c=.d)

clean:
	rm -rf demo c_demo *.o *.d *.mdep *.ymi lib include

.PHONY: all modules clean
