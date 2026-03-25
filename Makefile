# --------------------------------------------------------------------------
#
# Tiny C Compiler Makefile
#

ifndef TOP
 TOP = .
 INCLUDED = no
endif

ifeq ($(findstring $(MAKECMDGOALS),clean distclean),)
 include $(TOP)/config.mak
endif

ifeq (-$(GCC_MAJOR)-$(findstring $(GCC_MINOR),56789)-,-4--)
 CFLAGS += -D_FORTIFY_SOURCE=0
endif

LIBTCC = libtcc.a
LIBTCC1 = libtcc1.a
LINK_LIBTCC =
LIBS =
CFLAGS += $(CPPFLAGS) -std=c11 -Wunused-function -Wno-declaration-after-statement -Werror
VPATH = $(TOPSRC) $(TOPSRC)/arch
-LTCC = $(TOP)/$(LIBTCC)

# Enable extra runtime-debug features (not for release builds).
# This is intentionally controlled by configure's --debug (CONFIG_debug=yes).
ifeq ($(CONFIG_debug),yes)
 CFLAGS += -DCONFIG_TCC_DEBUG
endif

ifdef CONFIG_WIN32
 CFG = -win
 ifneq ($(CONFIG_static),yes)
  LIBTCC = libtcc$(DLLSUF)
  LIBTCCDEF = libtcc.def
 endif
 ifneq ($(CONFIG_debug),yes)
  LDFLAGS += -s
 endif
 NATIVE_TARGET = $(ARCH)-win$(if $(findstring arm,$(ARCH)),ce,32)
else
 CFG = -unx
 LIBS+=-lm
 ifneq ($(CONFIG_ldl),no)
  LIBS+=-ldl
 endif
 ifneq ($(CONFIG_pthread),no)
  LIBS+=-lpthread
 endif
 # make libtcc as static or dynamic library?
 ifeq ($(CONFIG_static),no)
  LIBTCC=libtcc$(DLLSUF)
  export LD_LIBRARY_PATH := $(CURDIR)/$(TOP)
  ifneq ($(CONFIG_rpath),no)
    ifndef CONFIG_OSX
      LINK_LIBTCC += -Wl,-rpath,"$(libdir)"
    else
      # macOS doesn't support env-vars libdir out of the box - which we need for
      # `make test' when libtcc.dylib is used (configure --disable-static), so
      # we bake a relative path into the binary. $libdir is used after install.
      LINK_LIBTCC += -Wl,-rpath,"@executable_path/$(TOP)" -Wl,-rpath,"$(libdir)"
      # -current/compatibility_version must not contain letters.
      MACOS_DYLIB_VERSION := $(firstword $(subst rc, ,$(VERSION)))
      DYLIBVER += -current_version $(MACOS_DYLIB_VERSION)
      DYLIBVER += -compatibility_version $(MACOS_DYLIB_VERSION)
    endif
  endif
 endif
 NATIVE_TARGET = $(ARCH)
 ifdef CONFIG_OSX
  NATIVE_TARGET = $(ARCH)-osx
  ifneq ($(CC_NAME),tcc)
    LDFLAGS += -flat_namespace
    ifneq (1,$(shell expr $(GCC_MAJOR) ">=" 15))
      LDFLAGS += -undefined warning # depreciated in clang >= 15.0
    endif
  endif
  export MACOSX_DEPLOYMENT_TARGET := 10.6
 endif
endif

# run local version of tcc with local libraries and includes
TCCFLAGS-unx = -B$(TOP) -I$(TOPSRC)/include -I$(TOPSRC) -I$(TOP)
TCCFLAGS-win = -B$(TOPSRC)/win32 -I$(TOPSRC)/include -I$(TOPSRC) -I$(TOP) -L$(TOP)
TCCFLAGS = $(TCCFLAGS$(CFG))
TCC_LOCAL = $(TOP)/tcc$(EXESUF)
TCC = $(TCC_LOCAL) $(TCCFLAGS)

# run tests with the installed tcc instead
ifdef TESTINSTALL
  TCC_LOCAL = $(bindir)/tcc
  TCCFLAGS-unx = -I$(TOP)
  TCCFLAGS-win = -B$(bindir) -I$(TOP)
  -LTCC = $(libdir)/$(LIBTCC) $(LINK_LIBTCC)
endif

CFLAGS_P = $(CFLAGS) -pg -static -DCONFIG_TCC_STATIC -DTCC_PROFILE
LIBS_P = $(LIBS)
LDFLAGS_P = $(LDFLAGS)

DEF-arm-fpa        = -DTCC_TARGET_ARM
DEF-arm-fpa-ld     = -DTCC_TARGET_ARM -DLDOUBLE_SIZE=12
DEF-arm-vfp        = -DTCC_TARGET_ARM -DTCC_ARM_VFP
DEF-arm-eabi       = -DTCC_TARGET_ARM -DTCC_ARM_VFP -DTCC_ARM_EABI
DEF-arm-eabihf     = $(DEF-arm-eabi) -DTCC_ARM_HARDFLOAT
DEF-armv8m         = $(DEF-arm-eabihf) -DTCC_TARGET_ARM_THUMB -DTCC_TARGET_ARM_ARCHV8M

# --- armv8m paths ---
# The armv8m cross-compiler does NOT link newlib by default.
# Include/library paths are set by ./configure (--sysincludepaths,
# --libpaths, --crtprefix) and point to the YasOS rootfs.
# Build libc et al. with build_rootfs.sh and install into rootfs/
# before compiling applications.

ifeq ($(INCLUDED),no)
# --------------------------------------------------------------------------
# running top Makefile

PROGS = tcc$(EXESUF)
TCCLIBS = $(LIBTCCDEF) $(LIBTCC) $(LIBTCC1)
TCCDOCS = tcc.1 tcc-doc.html tcc-doc.info

# all: $(PROGS) $(TCCLIBS) $(TCCDOCS)

TCC_X = armv8m

# cross libtcc1.a targets to build
LIBTCC1_X = $(filter-out c67,$(TCC_X))
FP_LIBS_STAMP_DIR = $(TOP)/lib/fp/build
FP_LIBS_SRC_DEPS = $(shell find $(TOP)/lib/fp -type f \( -name 'Makefile' -o -name '*.[chS]' \) -print 2>/dev/null)
FP_LIBS_CROSS = $(foreach X,$(TCC_X),$(FP_LIBS_STAMP_DIR)/.$X-fp-libs.stamp)

# Checksum utility for detecting compiler changes
CHECKSUM_CMD = $(shell command -v sha256sum 2>/dev/null || command -v md5sum 2>/dev/null || echo "")

# When TinyCC itself is built with ASan, leak detection (LSan) may cause
# the compiler process to exit non-zero on teardown, breaking recursive
# builds that invoke the freshly built compiler (e.g. fp-libs).
# Disable leak detection for those nested invocations so the build can
# proceed while still keeping ASan instrumentation.
ifeq ($(CONFIG_asan),yes)
SAN_ENV = LSAN_OPTIONS=detect_leaks=0 ASAN_OPTIONS=detect_leaks=0
endif


PROGS_CROSS = $(foreach X,$(TCC_X),$X-tcc$(EXESUF))
LIBTCC1_CROSS = $(foreach X,$(LIBTCC1_X),$X-libtcc1.a)

$(info $(LIBTCC1_CROSS))
# build cross compilers & libs
cross: $(LIBTCC1_CROSS) $(PROGS_CROSS) $(FP_LIBS_CROSS)

# build specific cross compiler & lib
cross-%: %-tcc$(EXESUF) %-libtcc1.a ;

fp-libs: $(FP_LIBS_CROSS)

# Backwards-compatible aliases (won't rebuild if stamp is up-to-date)
%-fp-libs: $(FP_LIBS_STAMP_DIR)/.%-fp-libs.stamp

# Compiler checksum file (tracks when compiler binary actually changes)
$(FP_LIBS_STAMP_DIR)/.%-tcc.checksum: %-tcc$(EXESUF)
	@mkdir -p $(FP_LIBS_STAMP_DIR)
	@if [ -n "$(CHECKSUM_CMD)" ]; then \
		$(CHECKSUM_CMD) $< | awk '{print $$1}' > $@.tmp && \
		if [ -f $@ ] && [ "$$(cat $@)" = "$$(cat $@.tmp)" ]; then \
			rm -f $@.tmp; \
		else \
			mv $@.tmp $@; \
		fi; \
	else \
		touch $@; \
	fi

$(FP_LIBS_STAMP_DIR)/.%-fp-libs.stamp: $(FP_LIBS_STAMP_DIR)/.%-tcc.checksum $(FP_LIBS_SRC_DEPS) %-libtcc1.a
	@mkdir -p $(FP_LIBS_STAMP_DIR)
	@# Check if checksum changed - if so, clean and rebuild fplibs
	@if [ -f $(FP_LIBS_STAMP_DIR)/.$*-fp-libs.checksum.saved ]; then \
		if ! cmp -s $(FP_LIBS_STAMP_DIR)/.$*-fp-libs.checksum.saved $(FP_LIBS_STAMP_DIR)/.$*-tcc.checksum; then \
			echo "Compiler $*-tcc changed - cleaning and rebuilding fplibs"; \
			$(MAKE) --no-print-directory -C lib clean-fp-libs CROSS_TARGET=$*; \
		fi; \
	fi
	@rm -f $@
	@$(SAN_ENV) $(MAKE) --no-print-directory -C lib CROSS_TARGET=$* fp-libs && touch $@
	@# Also build shared FP libraries for YasOS dynamic linking
	@$(SAN_ENV) $(MAKE) --no-print-directory -C lib CROSS_TARGET=$* fp-libs-shared || true
	@# Save the checksum that was used for this build
	@cp $(abspath $(FP_LIBS_STAMP_DIR)/.$*-tcc.checksum) $(abspath $(FP_LIBS_STAMP_DIR)/.$*-fp-libs.checksum.saved)

install: ; @$(MAKE) --no-print-directory  install$(CFG)
install-strip: ; @$(MAKE) --no-print-directory  install$(CFG) CONFIG_strip=yes
uninstall: ; @$(MAKE) --no-print-directory uninstall$(CFG)

ifdef CONFIG_cross
all : cross
endif

# --------------------------------------------

T = $(or $(CROSS_TARGET),$(NATIVE_TARGET),unknown)
X = $(if $(CROSS_TARGET),$(CROSS_TARGET)-)

DEFINES += $(DEF-$T)
DEFINES += $(if $(ROOT-$T),-DCONFIG_SYSROOT="\"$(ROOT-$T)\"")
DEFINES += $(if $(CRT-$T),-DCONFIG_TCC_CRTPREFIX="\"$(CRT-$T)\"")
DEFINES += $(if $(LIB-$T),-DCONFIG_TCC_LIBPATHS="\"$(LIB-$T)\"")
DEFINES += $(if $(INC-$T),-DCONFIG_TCC_SYSINCLUDEPATHS="\"$(INC-$T)\"")
DEFINES += $(if $(ELF-$T),-DCONFIG_TCC_ELFINTERP="\"$(ELF-$T)\"")
DEFINES += $(DEF-$(or $(findstring win,$T),unx))

ifneq ($(X),)
$(if $(DEF-$T),,$(error error: unknown target: '$T'))
DEF-$(NATIVE_TARGET) =
DEF-$T += -DCONFIG_TCC_CROSSPREFIX="\"$X\""
ifneq ($(CONFIG_WIN32),yes)
DEF-win += -DCONFIG_TCCDIR="\"$(tccdir)/win32\""
endif
else
# using values from config.h
DEF-$(NATIVE_TARGET) =
endif

# include custom configuration (see make help)
-include config-extra.mak

ifneq ($(T),$(NATIVE_TARGET))
# assume support files for cross-targets in "/usr/<triplet>" by default
MARCH-$T ?= $(TRIPLET-$T)
TR = $(if $(TRIPLET-$T),$T,ignored)
CRT-$(TR) ?= /usr/$(TRIPLET-$T)/lib
LIB-$(TR) ?= {B}:/usr/$(TRIPLET-$T)/lib:/usr/lib/$(MARCH-$T)
INC-$(TR) ?= {B}/include:/usr/$(TRIPLET-$T)/include:/usr/include
endif

IR_FILES = ir/type.c ir/pool.c ir/vreg.c ir/stack.c ir/live.c ir/dump.c ir/codegen.c ir/opt.c ir/opt_jump_thread.c ir/licm.c ir/core.c ir/machine_op.c
CORE_FILES = tccir_operand.c tccls.c tcc.c tcctools.c libtcc.c tccpp.c tccgen.c tccdbg.c tccelf.c tccasm.c tccyaff.c tccld.c tccdebug.c svalue.c tccmachine.c tccopt.c $(IR_FILES)
CORE_FILES += tcc.h config.h libtcc.h tcctok.h tccir.h tccir_operand.h tccld.h tccmachine.h tccopt.h
CORE_FILES += $(wildcard ir/*.h)
armv8m_FILES = $(CORE_FILES) arch/arm_aapcs.c arch/armv8m.c arm-thumb-opcodes.c arm-thumb-gen.c arm-thumb-callsite.c arm-link.c arm-thumb-asm.c arm-thumb-defs.h thumb-tok.h

TCCDEFS_H$(subst yes,,$(CONFIG_predefs)) = tccdefs_.h

# libtcc sources
LIBTCC_SRC = $(filter-out tcc.c tcctools.c,$(filter %.c,$($T_FILES)))

# Compile from separate objects
LIBTCC_OBJ = $(patsubst %.c,$(X)%.o,$(LIBTCC_SRC))
LIBTCC_INC = $(filter %.h %-gen.c %-link.c,$($T_FILES))
TCC_FILES = $(X)tcc.o $(LIBTCC_OBJ)
$(X)tccpp.o : $(TCCDEFS_H)

DEFINES += -I$(TOP) -I$(TOP)/ir

GITHASH:=$(shell git rev-parse --abbrev-ref HEAD 2>/dev/null || echo no)
ifneq ($(GITHASH),no)
GITHASH:=$(shell git log -1 --date=short --pretty='format:%cd $(GITHASH)@%h')
GITMODF:=$(shell git diff --quiet || echo '*')
DEF_GITHASH:= -DTCC_GITHASH="\"$(GITHASH)$(GITMODF)\""
endif

ifeq ($(CONFIG_debug),yes)
CFLAGS += -g
LDFLAGS += -g
endif

# convert "include/tccdefs.h" to "tccdefs_.h"
%_.h : include/%.h conftest.c
	# todo: how to pass host CC there?
	gcc -DC2STR $(filter %.c,$^) -o c2str.exe && ./c2str.exe $< $@

# target specific object rules
$(X)%.o : %.c $(LIBTCC_INC)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

$(X)arch/%.o : arch/%.c $(LIBTCC_INC)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

$(X)ir/%.o : ir/%.c $(LIBTCC_INC)
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

# additional dependencies
$(X)tcc.o : tcctools.c
$(X)tcc.o : DEFINES += $(DEF_GITHASH)

# Host Tiny C Compiler
# tcc$(EXESUF): tcc.o $(LIBTCC)
# 	$S$(CC) -o $@ $^ $(addsuffix ,$(LIBS) $(LDFLAGS) $(LINK_LIBTCC))

# Cross Tiny C Compilers
# (the TCCDEFS_H dependency is only necessary for parallel makes,
# ala 'make -j x86_64-tcc i386-tcc tcc', which would create multiple
# c2str.exe and tccdefs_.h files in parallel, leading to access errors.
# This forces it to be made only once.  Make normally tracks multiple paths
# to the same goals and only remakes it once, but that doesn't work over
# sub-makes like in this target)
%-tcc$(EXESUF): $(TCCDEFS_H) FORCE
	@$(MAKE) --no-print-directory $@ CROSS_TARGET=$*

$(CROSS_TARGET)-tcc$(EXESUF): $(TCC_FILES)
	$S$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

# Cross libtcc1.a
%-libtcc1.a : %-tcc$(EXESUF) FORCE
	@$(MAKE) -C lib CROSS_TARGET=$*

.PRECIOUS: %-libtcc1.a
FORCE:

# WHICH = which $1 2>/dev/null
# some versions of gnu-make do not recognize 'command' as a shell builtin
WHICH = sh -c 'command -v $1'

run-if = $(if $(shell $(call WHICH,$1)),$S $1 $2)
S = $(if $(findstring yes,$(SILENT)),@$(info * $@))

# --------------------------------------------------------------------------
# documentation and man page
tcc-doc.html: tcc-doc.texi
	$(call run-if,makeinfo,--no-split --html --number-sections -o $@ $<)

tcc-doc.info: tcc-doc.texi
	$(call run-if,makeinfo,$< || true)

tcc.1 : tcc-doc.pod
	$(call run-if,pod2man,--section=1 --center="Tiny C Compiler" \
		--release="$(VERSION)" $< >$@)
%.pod : %.texi
	$(call run-if,perl,$(TOPSRC)/texi2pod.pl $< $@)

doc : $(TCCDOCS)

# --------------------------------------------------------------------------
# install

INSTALL = install -m644
INSTALLBIN = install -m755 $(STRIP_$(CONFIG_strip))
STRIP_yes = -s

LIBTCC1_W = $(filter %-win32-libtcc1.a %-wince-libtcc1.a,$(LIBTCC1_CROSS))
LIBTCC1_U = $(filter-out $(LIBTCC1_W),$(wildcard *-libtcc1.a))
IB = $(if $1,$(IM) mkdir -p $2 && $(INSTALLBIN) $1 $2)
IBw = $(call IB,$(wildcard $1),$2)
IF = $(if $1,$(IM) mkdir -p $2 && $(INSTALL) $1 $2)
IFw = $(call IF,$(wildcard $1),$2)
IR = $(IM) mkdir -p $2 && cp -r $1/. $2
IM = @echo "-> $2 : $1" ;
BINCHECK = $(if $(wildcard $(PROGS) *-tcc$(EXESUF)),,@echo "Makefile: nothing found to install" && exit 1)

EXTRA_O = runmain.o bt-exe.o bt-dll.o bt-log.o bcheck.o

# install progs & libs
install-unx:
	$(call BINCHECK)
	$(call IBw,$(PROGS) *-tcc,"$(bindir)")
	$(call IFw,$(LIBTCC1) $(EXTRA_O) $(LIBTCC1_U),"$(tccdir)")
	$(call IFw,$(TOPSRC)/lib/fp/libtcc1-fp-*.a,"$(tccdir)/fp")
	$(call IFw,$(TOPSRC)/lib/fp/libsoftfp.a $(TOPSRC)/lib/fp/libvfpv4sp.a $(TOPSRC)/lib/fp/libvfpv5dp.a $(TOPSRC)/lib/fp/librp2350fp.a,"$(tccdir)/fp")
	$(call IFw,$(TOPSRC)/lib/fp/libsoftfp.so $(TOPSRC)/lib/fp/libvfpv4sp.so $(TOPSRC)/lib/fp/libvfpv5dp.so $(TOPSRC)/lib/fp/librp2350fp.so,"$(tccdir)/fp")
	$(call IFw,$(TOPSRC)/lib/fp/libsoftfp.a $(TOPSRC)/lib/fp/libvfpv4sp.a $(TOPSRC)/lib/fp/libvfpv5dp.a $(TOPSRC)/lib/fp/librp2350fp.a,"$(libdir)")
	$(call IFw,$(TOPSRC)/lib/fp/libsoftfp.so $(TOPSRC)/lib/fp/libvfpv4sp.so $(TOPSRC)/lib/fp/libvfpv5dp.so $(TOPSRC)/lib/fp/librp2350fp.so,"$(libdir)")
	$(call IF,$(TOPSRC)/include/*.h $(TOPSRC)/tcclib.h,"$(tccdir)/include")
	$(call $(if $(findstring .so,$(LIBTCC)),IBw,IFw),$(LIBTCC),"$(libdir)")
	$(call IF,$(TOPSRC)/libtcc.h,"$(includedir)")
	$(call IFw,tcc.1,"$(mandir)/man1")
	$(call IFw,tcc-doc.info,"$(infodir)")
	$(call IFw,tcc-doc.html,"$(docdir)")
ifneq "$(wildcard $(LIBTCC1_W))" ""
	$(call IFw,$(TOPSRC)/win32/lib/*.def $(LIBTCC1_W),"$(tccdir)/win32/lib")
	$(call IR,$(TOPSRC)/win32/include,"$(tccdir)/win32/include")
	$(call IF,$(TOPSRC)/include/*.h $(TOPSRC)/tcclib.h,"$(tccdir)/win32/include")
endif

# uninstall
uninstall-unx:
	@rm -fv $(addprefix "$(bindir)/",$(PROGS) $(PROGS_CROSS))
	@rm -fv $(addprefix "$(libdir)/", libtcc*.a libtcc*.so libtcc.dylib,$P)
	@rm -fv $(addprefix "$(includedir)/", libtcc.h)
	@rm -fv "$(mandir)/man1/tcc.1" "$(infodir)/tcc-doc.info"
	@rm -fv "$(docdir)/tcc-doc.html"
	@rm -frv "$(tccdir)"

# --------------------------------------------------------------------------
# other stuff

TAGFILES = *.[ch] include/*.h lib/*.[chS]
tags : ; ctags $(TAGFILES)
# cannot have both tags and TAGS on windows
ETAGS : ; etags $(TAGFILES)

# create release tarball from *current* git branch (including tcc-doc.html
# and converting two files to CRLF)
TCC-VERSION = tcc-$(VERSION)
TCC-VERSION = tinycc-mob-$(shell git rev-parse --short=7 HEAD)
tar:    tcc-doc.html
	mkdir -p $(TCC-VERSION)
	( cd $(TCC-VERSION) && git --git-dir ../.git checkout -f )
	cp tcc-doc.html $(TCC-VERSION)
	for f in tcc-win32.txt build-tcc.bat ; do \
	    cat win32/$$f | sed 's,\(.*\),\1\r,g' > $(TCC-VERSION)/win32/$$f ; \
	done
	tar cjf $(TCC-VERSION).tar.bz2 $(TCC-VERSION)
	rm -rf $(TCC-VERSION)
	git reset

config.mak:
	$(if $(wildcard $@),,@echo "Please run ./configure." && exit 1)

#+#+#+#+-----------------------------------------------------------------------
# run all tests
PYTHON ?= python3
PYTEST ?= pytest

# Pytest parallel workers: make test J=16 → pytest -n 16 (default: auto)
J ?= auto

# If set to 1, wrap compiler invocations with valgrind to detect memory errors.
# Usage: make test VALGRIND=1
VALGRIND ?= 0
ifeq ($(VALGRIND),1)
export CC_WRAPPER := valgrind --error-exitcode=99 --errors-for-leak-kinds=none --leak-check=no --track-origins=yes -q
endif

# If set to 1 (default), `make test` will create a local virtualenv and install
# Python requirements for tests/ir_tests before invoking pytest.
USE_VENV ?= 1
VENV_DIR ?= .venv
VENV_BINDIR := $(CURDIR)/$(VENV_DIR)/bin
VENV_PY := $(VENV_BINDIR)/python
VENV_PIP := $(VENV_BINDIR)/pip

IRTESTS_DIR := tests/ir_tests
IRTESTS_REQUIREMENTS := $(IRTESTS_DIR)/requirements.txt
IRTESTS_VENV_STAMP := $(VENV_DIR)/.irtests-requirements.stamp

NEWLIB_DIR := $(IRTESTS_DIR)/qemu/mps2-an505/newlib_build/arm-none-eabi/newlib
NEWLIB_LIBC_A := $(NEWLIB_DIR)/libc.a

# Host tests for soft-float aeabi functions
AEABI_HOST_TESTS = test_aeabi_all test_host test_dmul_host
AEABI_HOST_TEST_DIR = lib/fp/soft

test-aeabi-host:
	@echo "------------ aeabi host tests ------------"
	@for t in $(AEABI_HOST_TESTS); do \
		echo "Building and running $$t..."; \
		$(CC) -O2 -DHOST_TEST $(AEABI_HOST_TEST_DIR)/$$t.c -o $(AEABI_HOST_TEST_DIR)/$$t -lm && \
		$(AEABI_HOST_TEST_DIR)/$$t || exit 1; \
	done
	@echo "------------ aeabi host tests passed ------------"

.PHONY: test-venv
test-venv:
	@set -e; \
	if [ "$(USE_VENV)" != "1" ]; then exit 0; fi; \
	if [ ! -f "$(IRTESTS_REQUIREMENTS)" ]; then echo "Missing $(IRTESTS_REQUIREMENTS)"; exit 1; fi; \
	$(MAKE) --no-print-directory $(IRTESTS_VENV_STAMP)

$(IRTESTS_VENV_STAMP): $(IRTESTS_REQUIREMENTS)
	@set -e; \
	if [ "$(USE_VENV)" != "1" ]; then exit 0; fi; \
	if [ ! -x "$(VENV_PY)" ]; then \
		echo "------------ ir_tests: creating venv ($(VENV_DIR)) ------------"; \
		$(PYTHON) -m venv "$(VENV_DIR)"; \
	fi; \
	echo "------------ ir_tests: installing python deps ------------"; \
	"$(VENV_PY)" -m pip install -U pip; \
	"$(VENV_PY)" -m pip install -r "$(IRTESTS_REQUIREMENTS)"; \
	touch "$@"

.PHONY: test-prepare
test-prepare:
	@set -e; \
	if [ -f "$(NEWLIB_LIBC_A)" ]; then exit 0; fi; \
	echo "------------ ir_tests: building newlib (first run) ------------"; \
	cd $(IRTESTS_DIR)/qemu/mps2-an505 && sh ./build_newlib.sh


ASMTESTS_DIR := tests/thumb/armv8m

.PHONY: test-asm
test-asm: cross test-venv
	@echo "------------ assembler tests (pytest) ------------"
	@set -e; \
	cd $(ASMTESTS_DIR) && \
		TEST_CC="$(CURDIR)/armv8m-tcc"; \
		TEST_COMPARE_CC="arm-none-eabi-gcc"; \
		TEST_OBJDUMP="arm-none-eabi-objdump"; \
		TEST_OBJCOPY="arm-none-eabi-objcopy"; \
		export TEST_CC TEST_COMPARE_CC TEST_OBJDUMP TEST_OBJCOPY; \
		if [ "$(USE_VENV)" = "1" ]; then \
			"$(VENV_PY)" -m pytest --tb=short -q -n $(J) .; \
		else \
			$(PYTEST) --tb=short -q -n $(J) .; \
		fi

# Check that cross-compilation produces no unexpected warnings or errors.
# Rebuilds libtcc1.a and compiles test files with -c, failing on any
# "warning:" or "error:" in stderr.
WARN_CHECK_SRCS = \
	tests/tests2/15_recursion.c \
	tests/tests2/14_if.c \
	tests/tests2/04_for.c \
	tests/tests2/08_while.c \
	tests/tests2/09_do_while.c \
	tests/tests2/06_case.c \
	tests/tests2/07_function.c

.PHONY: warn-check
warn-check: armv8m-tcc$(EXESUF)
	@echo "------------ warn-check: libtcc1.a build ------------"
	@rm -f armv8m-libtcc1.a
	@log=$$($(MAKE) --no-print-directory armv8m-libtcc1.a 2>&1) ; \
	warns=$$(echo "$$log" | grep -c -E 'warning:|error:') ; \
	if [ "$$warns" -ne 0 ]; then \
		echo "FAIL: unexpected warnings/errors building libtcc1.a:" ; \
		echo "$$log" | grep -E 'warning:|error:' ; \
		exit 1 ; \
	fi
	@echo "------------ warn-check: test file compilation ------------"
	@fail=0 ; \
	for f in $(WARN_CHECK_SRCS); do \
		out=$$(./armv8m-tcc$(EXESUF) -c "$$f" -o /dev/null 2>&1) ; \
		if echo "$$out" | grep -qE 'warning:|error:'; then \
			echo "FAIL: $$f:" ; \
			echo "$$out" | grep -E 'warning:|error:' ; \
			fail=1 ; \
		fi ; \
	done ; \
	if [ "$$fail" -ne 0 ]; then exit 1; fi
	@echo "------------ warn-check: passed ------------"

# run IR tests via pytest (preferred)
test: cross test-aeabi-host test-asm warn-check test-venv test-prepare download-gcc-tests
	@echo "------------ ir_tests (pytest) ------------"
	@if [ "$(USE_VENV)" = "1" ]; then \
		cd $(IRTESTS_DIR) && "$(VENV_PY)" -m pytest -s -n $(J); \
	else \
		cd $(IRTESTS_DIR) && $(PYTEST) -s -n $(J); \
	fi

# legacy tests (kept for reference)
test-legacy:
	@$(MAKE) -C tests
# run test(s) from tests2 subdir (see make help)
tests2.%:
	@$(MAKE) -C tests/tests2 $@
# run test(s) from testspp subdir (see make help)
testspp.%:
	@$(MAKE) -C tests/pp $@
# run tests with code coverage
tcov-tes% : tcc_c$(EXESUF)
	@rm -f $<.tcov
	@$(MAKE) --no-print-directory TCC_LOCAL=$(CURDIR)/$< tes$*
tcc_c$(EXESUF): $($T_FILES)
	$S$(TCC) tcc.c -o $@ -ftest-coverage $(DEFINES) $(LIBS)
# test the installed tcc instead
test-install: $(TCCDEFS_H)
	@$(MAKE) -C tests TESTINSTALL=yes #_all

clean:
	@rm -f tcc *-tcc tcc_p tcc_c
	@rm -f tags ETAGS *.o *.a *.so* *.out *.log lib*.def *.exe *.dll
	@rm -f a.out *.dylib *_.h *.pod *.tcov
	@$(MAKE) -s -C lib $@
	@$(MAKE) -s -C tests $@

distclean: clean
	@rm -vf config.h config.mak config.texi
	@rm -vf $(TCCDOCS)

# unified tests2 test suite
test-tests2: cross test-venv
	@echo "------------ tests2 test suite ------------"
	@if [ "$(USE_VENV)" = "1" ]; then \
		cd $(TOP)/tests && "$(VENV_PY)" run_tests.py --tests2 -v -n $(J); \
	else \
		cd $(TOP)/tests && $(PYTEST) -v -m tests2 --tb=short -n $(J) tests/tests2/; \
	fi

# download GCC torture tests
download-gcc-tests:
	@echo "------------ downloading GCC torture tests ------------"
	@bash $(TOP)/tests/gcctestsuite/download_gcc_tests.sh

# run GCC torture compile tests (compile only, via ir_tests framework)
test-gcc-torture-compile: cross test-venv test-prepare download-gcc-tests
	@echo "------------ GCC torture compile tests ------------"
	@if $(PYTEST) --help 2>/dev/null | grep -q timeout; then \
		PYTEST_TIMEOUT="--timeout=60"; \
	else \
		PYTEST_TIMEOUT=""; \
	fi; \
	if [ "$(USE_VENV)" = "1" ]; then \
		cd $(IRTESTS_DIR) && "$(VENV_PY)" -m pytest -m "gcc_compile" --tb=short -n $(J) $$PYTEST_TIMEOUT test_gcc_torture_ir.py; \
	else \
		cd $(IRTESTS_DIR) && $(PYTEST) -m "gcc_compile" --tb=short -n $(J) $$PYTEST_TIMEOUT test_gcc_torture_ir.py; \
	fi

# run GCC torture execute tests only (via ir_tests framework)
test-gcc-torture-execute: cross test-venv test-prepare download-gcc-tests
	@echo "------------ GCC torture execute tests ------------"
	@if $(PYTEST) --help 2>/dev/null | grep -q timeout; then \
		PYTEST_TIMEOUT="--timeout=120"; \
	else \
		PYTEST_TIMEOUT=""; \
	fi; \
	if [ "$(USE_VENV)" = "1" ]; then \
		cd $(IRTESTS_DIR) && "$(VENV_PY)" -m pytest -m "gcc_execute" --tb=short -n $(J) $$PYTEST_TIMEOUT test_gcc_torture_ir.py; \
	else \
		cd $(IRTESTS_DIR) && $(PYTEST) -m "gcc_execute" --tb=short -n $(J) $$PYTEST_TIMEOUT test_gcc_torture_ir.py; \
	fi

# run full GCC torture tests (compile + execute via ir_tests framework)
test-gcc-torture: cross test-venv test-prepare download-gcc-tests
	@echo "------------ GCC torture tests (compile + execute) ------------"
	@if $(PYTEST) --help 2>/dev/null | grep -q timeout; then \
		PYTEST_TIMEOUT="--timeout=120"; \
	else \
		PYTEST_TIMEOUT=""; \
	fi; \
	if [ "$(USE_VENV)" = "1" ]; then \
		cd $(IRTESTS_DIR) && "$(VENV_PY)" -m pytest -m "gcc_torture" --tb=short -n $(J) $$PYTEST_TIMEOUT test_gcc_torture_ir.py; \
	else \
		cd $(IRTESTS_DIR) && $(PYTEST) -m "gcc_torture" --tb=short -n $(J) $$PYTEST_TIMEOUT test_gcc_torture_ir.py; \
	fi

# run full test suite (IR + GCC torture compile-only)
# Note: tests2 tests are included in IR tests via test_qemu.py
test-full: cross test-aeabi-host test-asm test-venv test-prepare test-gcc-torture-compile
	@echo "------------ full test suite complete ------------"

# run all tests including full GCC torture (IR + GCC torture compile + execute)
test-all: cross test-aeabi-host test-asm test-venv test-prepare test-gcc-torture
	@echo "------------ unified test runner (IR + full GCC torture) ------------"

# convenience: run IR tests under valgrind
test-valgrind:
	$(MAKE) test VALGRIND=1

.PHONY: all cross fp-libs clean test test-valgrind test-aeabi-host test-legacy test-tests2 test-gcc-torture test-gcc-torture-compile test-gcc-torture-execute test-full test-all download-gcc-tests tar tags ETAGS doc distclean install uninstall FORCE

# Container image settings (auto-detect docker or podman)
DOCKER_REGISTRY ?= ghcr.io
DOCKER_IMAGE_NAME ?= matgla/tinycc-armv8m
DOCKER_IMAGE_TAG ?= latest
DOCKER_FULL_IMAGE = $(DOCKER_REGISTRY)/$(DOCKER_IMAGE_NAME):$(DOCKER_IMAGE_TAG)

# Detect available container runtime (prefer podman, fallback to docker)
# User can override with: make docker-start CONTAINER_RUNTIME=docker
CONTAINER_RUNTIME := $(shell \
  if command -v podman >/dev/null 2>&1 && podman info >/dev/null 2>&1; then \
    echo podman; \
  elif command -v docker >/dev/null 2>&1; then \
    echo docker; \
  fi)
# Note: Container runtime is only needed for container-* and docker-* targets

container-build:
ifeq ($(CONTAINER_RUNTIME),)
	$(error No container runtime found. Please install docker or podman.)
else
	@echo "Building container image with $(CONTAINER_RUNTIME): $(DOCKER_FULL_IMAGE)"
	$(CONTAINER_RUNTIME) build -t $(DOCKER_FULL_IMAGE) .
endif

container-push: container-build
ifeq ($(CONTAINER_RUNTIME),)
	$(error No container runtime found. Please install docker or podman.)
else
	@echo "Pushing container image with $(CONTAINER_RUNTIME): $(DOCKER_FULL_IMAGE)"
	$(CONTAINER_RUNTIME) push $(DOCKER_FULL_IMAGE)
endif

# Legacy aliases for backwards compatibility
docker-build: container-build
docker-push: container-push

# Pull and start container interactively with current directory mounted
docker-start:
ifeq ($(CONTAINER_RUNTIME),)
	$(error No container runtime found. Please install docker or podman.)
else
	@echo "Pulling container image with $(CONTAINER_RUNTIME): $(DOCKER_FULL_IMAGE)"
	$(CONTAINER_RUNTIME) pull $(DOCKER_FULL_IMAGE)
	@echo "Starting container with $(CONTAINER_RUNTIME)..."
	$(CONTAINER_RUNTIME) run -it --rm -v $(CURDIR):/workspace $(DOCKER_FULL_IMAGE)
endif

help:
	@echo "make"
	@echo "   build native compiler (from separate objects)"
	@echo "make cross"
	@echo "   build cross compilers (from separate objects)"
	@echo "make SILENT=no/yes"
	@echo "   build less/more silently"
	@echo "make cross-TARGET"
	@echo "   build one specific cross compiler for 'TARGET'. Currently supported:"
	@echo "   $(wordlist 1,8,$(TCC_X))"
	@echo "   $(wordlist 9,99,$(TCC_X))"
	@echo "make test"
	@echo "   rebuild + initialize GCC testsuite + run pytest in tests/ir_tests"
	@echo "make test-legacy"
	@echo "   run legacy make-based tests (tests/Makefile)"
	@echo "make tests2.all / make tests2.37 / make tests2.37+"
	@echo "   run all/single test(s) from tests2, optionally update .expect"
	@echo "make testspp.all / make testspp.17"
	@echo "   run all/single test(s) from tests/pp"
	@echo "make tcov-test / tcov-tests2... / tcov-testspp..."
	@echo "   run tests as above with code coverage. After test(s) see tcc_c$(EXESUF).tcov"
	@echo "make test-install"
	@echo "   run tests with the installed tcc"
	@echo "Other supported make targets:"
	@echo "   install install-strip uninstall doc [dist]clean tags ETAGS tar help"
	@echo "   container-build"
	@echo "      build container image (auto-detects docker/podman)"
	@echo "   container-push"
	@echo "      build and push container image to registry"
	@echo "   docker-build (legacy alias)"
	@echo "   docker-push (legacy alias)"
	@echo "   docker-start"
	@echo "      pull and start container interactively (mounts current dir to /workspace)"
	@echo "Custom configuration:"
	@echo "   The makefile includes a file 'config-extra.mak' if it is present."
	@echo "   This file may contain some custom configuration.  For example to"
	@echo "   configure the search paths for a cross-compiler, assuming the"
	@echo "   support files in /usr/i686-linux-gnu:"
	@echo "      ROOT-i386 = /usr/i686-linux-gnu"
	@echo "      CRT-i386  = {R}/lib"
	@echo "      LIB-i386  = {B}:{R}/lib"
	@echo "      INC-i386  = {B}/include:{R}/include (*)"
	@echo "      DEF-i386  += -D__linux__"
	@echo "   Or also, for the cross platform files in /usr/<triplet>"
	@echo "      TRIPLET-i386 = i686-linux-gnu"
	@echo "   (*) tcc replaces {B} by 'tccdir' and {R} by 'CONFIG_SYSROOT'"

# --------------------------------------------------------------------------
endif # ($(INCLUDED),no)
