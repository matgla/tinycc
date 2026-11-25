# Catching bugs with tinycc: the plain-C examples (README.md, docs/checks.md).
#
#   make            build the programs
#   make run        build and run them (ub traps on purpose: run it last)
#   make bugs       compile chapter 1's buggy file, which is meant to warn
#   make stack      chapter 2's warnings, then the call-graph report
#                   (scripts/stack_usage.py runs on the host: python3)
#
# Every file here is regular C, so every command says -ccompat.

TCC ?= tcc
CFLAGS ?= -O2
PYTHON ?= python3
STACK_USAGE ?= ../../scripts/stack_usage.py

# the safe default's checks for C, by name (docs/checks.md, "Plain C")
CHECKS = -ccompat -Wall -Wextra -Wuninitialized -Wmaybe-uninitialized \
         -Wdangling-pointer -Wnull-dereference -Wformat-security
STACK = -ccompat -Wframe-larger-than=1024 -Wstack-usage=1024 -Wvla \
        -fstack-usage -fcallgraph-info=su

PROGS = fixed stack_budget ub ub_report

all: $(PROGS)

bugs:
	$(TCC) $(CFLAGS) $(CHECKS) -c 01_find_the_bugs.c -o 01_find_the_bugs.o

fixed: 01_fixed.c
	$(TCC) $(CFLAGS) $(CHECKS) -Werror 01_fixed.c -o $@

02_stack_budget.o: 02_stack_budget.c
	$(TCC) $(CFLAGS) $(STACK) -c 02_stack_budget.c -o $@

stack: 02_stack_budget.o
	$(PYTHON) $(STACK_USAGE) --libtcc1 --unknown-budget 256 \
	    -e run_unbounded -e run_bounded 02_stack_budget.ci

stack_budget: 02_stack_budget.o
	$(TCC) 02_stack_budget.o -o $@

# trap mode: no runtime needed, the first bad step stops the program
ub: 03_sanitizers.c
	$(TCC) $(CFLAGS) -ccompat -fsanitize=undefined 03_sanitizers.c -o $@

# handler mode: libc's __ubsan_handle_* report each step and go on
ub_report: 03_sanitizers.c
	$(TCC) $(CFLAGS) -ccompat -fsanitize=undefined -fsanitize-recover=undefined \
	    03_sanitizers.c -o $@

run: all
	for p in fixed stack_budget ub_report ub; do echo "== $$p"; ./$$p; done

clean:
	rm -f $(PROGS) *.o *.su *.ci

.PHONY: all bugs stack run clean
