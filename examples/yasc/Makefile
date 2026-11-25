# The yasc tutorial's programs (README.md).
#
#   make            build every chapter's program
#   make run        build and run them
#   make mistakes   compile chapter 3, which is meant to warn
#
# tcc compiles yasc by default (safe mode); the regular C halves (mem.c,
# counter.c, slots.c) say -ccompat.

TCC ?= tcc
CFLAGS ?= -O1
PROGS = hello names defer errors counter methods config arrays

all: $(PROGS)

mem.o: mem.c mem.h
	$(TCC) $(CFLAGS) -ccompat -c mem.c -o $@

slots.o: slots.c
	$(TCC) $(CFLAGS) -ccompat -c slots.c -o $@

counter.o: counter.c counter.h
	$(TCC) $(CFLAGS) -ccompat -c counter.c -o $@

hello: 01_hello.c
	$(TCC) $(CFLAGS) 01_hello.c -o $@

names: 02_ownership.c mem.o
	$(TCC) $(CFLAGS) 02_ownership.c mem.o -o $@

defer: 04_defer.c mem.o
	$(TCC) $(CFLAGS) 04_defer.c mem.o -o $@

errors: 05_errors.c mem.o
	$(TCC) $(CFLAGS) 05_errors.c mem.o -o $@

counter: 06_c_library.c counter.o
	$(TCC) $(CFLAGS) 06_c_library.c counter.o -o $@

methods: 07_methods.c mem.o
	$(TCC) $(CFLAGS) 07_methods.c mem.o -o $@

config: 09_config.c 09_settings.txt mem.o
	$(TCC) $(CFLAGS) 09_config.c mem.o -o $@

arrays: 10_arrays.c slots.h mem.o slots.o
	$(TCC) $(CFLAGS) 10_arrays.c mem.o slots.o -o $@

mistakes:
	$(TCC) -O1 -c 03_mistakes.c -o 03_mistakes.o

run: all
	for p in $(PROGS); do echo "== $$p"; ./$$p; done

clean:
	rm -f $(PROGS) *.o

.PHONY: all run mistakes clean
