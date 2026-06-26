# Frontend coverage tests for ARMv8-M TinyCC.
#
# Mirrors tests/pp/Makefile but drives the pytest harness instead of raw
# preprocessor invocations.

TOP = ../..
TCC = $(TOP)/bin/armv8m-tcc
PYTHON = python3

.PHONY: all test update pp types diagnostics clean

all test:
	$(PYTHON) -m pytest $(CURDIR) -q

pp:
	$(PYTHON) -m pytest $(CURDIR)/test_frontend.py -q -m frontend_pp

types:
	$(PYTHON) -m pytest $(CURDIR)/test_frontend.py -q -m frontend_types

diagnostics:
	$(PYTHON) -m pytest $(CURDIR)/test_frontend.py -q -m frontend_diagnostics

update:
	$(PYTHON) -m pytest $(CURDIR) --update -q

clean:
	find $(CURDIR) -name '*.o' -delete
