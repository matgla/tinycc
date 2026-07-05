# Orchestrator for tinycc unit tests.
#
# Unit tests live under per-target subdirectories (tests/unit/<arch>/<mcu>)
# so each build can mirror the preprocessor environment of the cross
# compiler it validates. This Makefile fans out across them.
#
# Add new targets by appending to UT_TARGETS and creating the matching
# directory with its own Makefile (model on arm/armv8m/Makefile).

UT_TARGETS := arm/armv8m

.PHONY: all run clean coverage $(UT_TARGETS)

all: $(UT_TARGETS)

$(UT_TARGETS):
	$(MAKE) -C $@ all

run:
	@set -e; for t in $(UT_TARGETS); do \
	    echo "==> $$t"; \
	    $(MAKE) --no-print-directory -C $$t run; \
	done

# gcov line/branch coverage of the tinycc modules under test. Does a clean
# instrumented build, runs each suite, and renders an HTML + text report under
# <target>/build/coverage/. Requires gcovr.
coverage:
	@set -e; for t in $(UT_TARGETS); do \
	    echo "==> $$t"; \
	    $(MAKE) --no-print-directory -C $$t coverage; \
	done

clean:
	@set -e; for t in $(UT_TARGETS); do \
	    $(MAKE) --no-print-directory -C $$t clean; \
	done
