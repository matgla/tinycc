# --------------------------------------------------------------------------
#
# Tiny C Compiler Makefile
#

ifndef TOP
 TOP = .
 INCLUDED = no
endif

NO_CONFIG_GOALS := clean distclean help \
	run start_env build_container pull_container push_container \
	container-build container-pull container-push \
	docker-build docker-push docker-start

ifeq ($(MAKECMDGOALS),)
 include $(TOP)/config.mak
else ifneq ($(filter-out $(NO_CONFIG_GOALS),$(MAKECMDGOALS)),)
 include $(TOP)/config.mak
endif

ifeq (-$(GCC_MAJOR)-$(findstring $(GCC_MINOR),56789)-,-4--)
 CFLAGS += -D_FORTIFY_SOURCE=0
endif

ENABLE_GC_SECTIONS ?= no
ENABLE_LTO ?= no
RELEASE ?= no

ifneq ($(filter 1 yes true,$(RELEASE)),)
 ENABLE_GC_SECTIONS := yes
 ENABLE_LTO := yes
 override CFLAGS := $(filter-out -g,$(CFLAGS))
 CFLAGS += -DNDEBUG
 LDFLAGS += -s
endif

ifneq ($(filter 1 yes true,$(ENABLE_GC_SECTIONS)),)
 CFLAGS += -ffunction-sections -fdata-sections
 LDFLAGS += -Wl,--gc-sections
endif

ifneq ($(filter 1 yes true,$(ENABLE_LTO)),)
 CFLAGS += -flto
 LDFLAGS += -flto
endif

LIBTCC = libtcc.a
LIBTCC1 = libtcc1.a
LINK_LIBTCC =
LIBS =
CFLAGS += $(CPPFLAGS) -std=c11 -Wunused-function -Wno-declaration-after-statement -Werror
VPATH = $(TOPSRC) $(TOPSRC)/source/backend/arch
-LTCC = $(TOP)/$(LIBTCC)

# Dump-IR support: the -dump-ir / -dump-ir-passes options and the per-pass IR
# dumps they drive (all guarded by CONFIG_TCC_DEBUG, which in this fork gates
# nothing but the IR-dump feature).  Enabled by default so IR tooling and the
# frontend golden-IR tests work with a plain `make cross`.  The dump calls are
# no-ops unless -dump-ir is passed, so this has no effect on generated code.
#
# For a smaller "minimal" release binary without the dump-IR machinery, build
# with CONFIG_minimal=yes (e.g. `make cross CONFIG_minimal=yes`).
ifneq ($(CONFIG_minimal),yes)
 CFLAGS += -DCONFIG_TCC_DEBUG
endif

# getenv-driven debug/bisect knobs (TCC_DISABLE_PASS, TCC_NO_COALESCE, DBG_CLINL,
# SCAN_OVERLAP, ...).  See tccdbgenv.h for the full rationale and for how to add
# one.  CONFIG_TCC_DEBUG_ENV defaults to CONFIG_TCC_DEBUG's setting, so a plain
# `make cross` keeps every knob and the host bisect tooling (bisect_pass.sh,
# scripts/opt_profile.py, reduce.py, tests/fuzz/triage_olevels.sh) works unchanged.
#
# CONFIG_debugenv=no compiles them all out: each knob folds to a compile-time
# constant, the code it guarded becomes dead, and the ~27 getenv lookups a
# compile makes drop to the 3 real ones (CPATH & friends).  Measured on the
# armv8m device compiler: .text -14,584 B, total -17,272 B, output byte-identical
# over 1,545 corpus compiles.  build_rootfs.sh passes this for the on-device
# compiler unless --debug-tcc was given.
ifeq ($(CONFIG_debugenv),no)
 CFLAGS += -DCONFIG_TCC_DEBUG_ENV=0
endif
ifeq ($(CONFIG_debugenv),yes)
 CFLAGS += -DCONFIG_TCC_DEBUG_ENV=1
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
# Leak detection (LSan) is enabled by default for `make test`: every compiler
# invocation runs the at-exit leak check, so any leak in tcc surfaces as a
# non-zero exit.  Note tcc (like most compilers) intentionally does not free
# everything on exit, so known pre-existing leaks will fail here too; override
# by exporting your own [AL]SAN_OPTIONS (e.g. detect_leaks=0) to opt out.
# The nested fp-libs build (SAN_ENV above) keeps leak detection off so the
# build can still complete.
export LSAN_OPTIONS ?= detect_leaks=1
export ASAN_OPTIONS ?= detect_leaks=1
endif


PROGS_CROSS = $(foreach X,$(TCC_X),$X-tcc$(EXESUF))
LIBTCC1_CROSS = $(foreach X,$(LIBTCC1_X),$X-libtcc1.a)
AUTO_PCH_COMMON_HEADERS = stdio.h stdlib.h string.h
AUTO_PCH_STAMPS = $(foreach X,$(TCC_X),$(TOP)/pch/.$X-auto-pch.stamp)

$(info $(LIBTCC1_CROSS))
# build cross compilers & libs
# PCH disabled on YasOS (unused; costs runtime heap + startup probe time) —
# auto-PCH generation dropped here.  Re-add $(AUTO_PCH_STAMPS) (and re-enable
# the loader in tccpp.c / pch_auto_enabled) to restore precompiled headers.
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

$(TOP)/pch/.%-auto-pch.stamp: %-tcc$(EXESUF)
	@mkdir -p "$(TOP)/pch/$*"
	@dir="$(abspath $(TOP)/pch/$*)"; \
	index="$$dir/auto.index"; \
	tool="./$*-tcc$(EXESUF) -B$(TOP)"; \
	rm -f "$$index"; \
	for hdr in $(AUTO_PCH_COMMON_HEADERS); do rm -f "$$dir/$$hdr.pch" "$$dir/$$hdr.opt.pch"; done; \
	includes="$$($$tool -print-search-dirs 2>/dev/null | awk 'BEGIN { in_include = 0 } /^include:$$/ { in_include = 1; next } /^[^ ]/ { if (in_include) exit } in_include { sub(/^  /, ""); if ($$0 != "-") print }' || true)"; \
	for hdr in $(AUTO_PCH_COMMON_HEADERS); do \
		src=""; \
		for inc in $$includes; do \
			if [ -f "$$inc/$$hdr" ]; then \
				src="$$inc/$$hdr"; \
				break; \
			fi; \
		done; \
		[ -n "$$src" ] || continue; \
		for opt in 0 1; do \
			if [ "$$opt" = 0 ]; then oflags=""; pch="$$hdr.pch"; else oflags="-O$$opt"; pch="$$hdr.opt.pch"; fi; \
			if $$tool $$oflags -generate-pch "$$src" -o "$$dir/$$pch" >/dev/null 2>&1; then \
				probe="$$dir/.$$hdr.probe.c"; \
				printf '#include <%s>\nint main(void){return 0;}\n' "$$hdr" > "$$probe"; \
				out="$$($$tool $$oflags -use-pch "$$dir/$$pch" -E "$$probe" 2>&1 >/dev/null || true)"; \
				rm -f "$$probe"; \
				if ! printf '%s' "$$out" | grep -q 'ignoring PCH'; then \
					printf '%s\t%s\n' "$$src" "$$pch" >> "$$index"; \
				else \
					rm -f "$$dir/$$pch"; \
				fi; \
			else \
				rm -f "$$dir/$$pch"; \
			fi; \
		done; \
	done; \
	touch "$@"

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

# The core dependency set for every object rule, here and in the included
# sub-Makefiles (which are read before CORE_FILES/LIBTCC_INC exist, so they
# cannot use those).
#
# Every TU in the tree includes tcc.h, and tcc.h reaches the rest of the shared
# headers: config.h directly, elf.h via tcctypes.h, and so on.  Naming them
# individually has gone stale repeatedly, so take the lot by wildcard -- one
# wildcard per module header directory, since the headers moved out of the repo
# root and in with the module that owns them.  config.mak is here for the
# *flags* (ASan, -g, -DCONFIG_TCC_DEBUG), which no header records.
#
# Getting this wrong is quiet and expensive.  config.h's CONFIG_TCC_BCHECK adds
# three members to struct TCCState; when a reconfigure enabled it, the three
# rules that listed neither config.h nor config.mak kept their old objects, so
# `gen_function` stored tcc_state->ir at offset 11328 while `vstore` read it
# from 11344.  Every compile then died in tcc_ir_put on a NULL IR pointer —
# after a `make` that reported nothing to do.
TCC_HDR_DIRS = source/include source/driver source/frontend source/ir \
	source/machine source/obj source/support
TCC_CORE_DEPS = $(wildcard *.h) \
	$(foreach d,$(TCC_HDR_DIRS),$(wildcard $(d)/*.h)) \
	config.mak

# Archive a module's objects into its library.  Shared by every module Makefile
# below so they all agree on how a lib is produced.
define ar-lib
@mkdir -p $(dir $@)
@rm -f $@
$S$(AR) rcs $@ $^
endef

# Module build configurations.  Order matters: a rule's prerequisite list is
# expanded when the rule is read, so a module has to be included after
# everything whose variables it names.  That is why each gen/ precedes its
# parent, and why source/opt/Makefile -- which collects the ssa/ and flat/ pass
# lists into one library -- comes last of the opt group.
include source/include/Makefile
include source/utils/Makefile
include source/memory/Makefile
include source/support/Makefile
include source/machine/Makefile
include source/obj/Makefile
include source/driver/Makefile
include source/ir/gen/Makefile
include source/ir/Makefile
include source/frontend/gen/Makefile
include source/frontend/Makefile
include source/opt/framework/Makefile
include source/opt/ssa/Makefile
include source/opt/flat/Makefile
include source/opt/Makefile
include source/backend/generators/Makefile

# The whole source set, assembled from what each module declares rather than
# restated here.  Feeds $T_FILES below, and the tags / coverage targets.
CORE_SRC = $(DRIVER_MAIN_SRC) $(DRIVER_SRC) $(FRONTEND_SRC) $(GEN_SRC) \
	$(IR_SRC) $(IR_GEN_FILES) $(MACHINE_SRC) $(OBJ_SRC) $(SUPPORT_SRC) \
	$(OPT_SRC) $(MEMORY_SRC) $(BACKEND_GENERATORS_SRC)

CORE_HDR = $(CORE_HDRS) $(DRIVER_HDRS) $(FRONTEND_HDRS) $(GEN_HDRS) \
	$(IR_HDRS) $(MACHINE_HDRS) $(OBJ_HDRS) $(SUPPORT_HDRS) $(MEMORY_HDRS) \
	$(UTILS_HDRS) $(wildcard source/opt/include/*.h) $(wildcard *.h)

CORE_FILES = $(CORE_SRC) $(CORE_HDR)
armv8m_FILES = $(CORE_FILES) source/backend/arch/arm/thumb/arm-thumb-defs.h source/backend/arch/arm/thumb/arm-thumb-callsite.h source/backend/arch/arm/thumb/thumb-tok.h source/backend/arch/arm/thumb/thumb.h source/backend/arch/arm/arm.h
armv8m_ARCH = arm
armv8m_ARCH_LIB = $(X)source/backend/arch/arm/libarm.a

TCCDEFS_H$(subst yes,,$(CONFIG_predefs)) = tccdefs_.h tccdecls_.h

# libtcc sources
LIBTCC_SRC = $(filter-out $(DRIVER_MAIN_SRC) source/driver/tcctools.c,$(filter %.c,$($T_FILES)))

# Compile from separate objects
LIBTCC_OBJ = $(patsubst %.c,$(X)%.o,$(LIBTCC_SRC))
LIBTCC_INC = $(filter %.h %-gen.c %-link.c,$($T_FILES))
ARCH_LIB = $($T_ARCH_LIB)

# Every module builds to its own static library; the executable is the CLI
# entry object plus those libs.
#
# MODULE_LIBS is linked --whole-archive, which is not decoration.  A normal
# archive scan only pulls the members that resolve an undefined symbol, and
# these modules hold TUs nothing references by design: the SValue/Sym
# pretty-printers exist to be called from gdb, and several opt passes are
# compiled but not yet wired into the pipeline table.  Linking the archives
# a la carte dropped 53 symbols (all of tccmachine.c, the pass registry, eight
# tcc_ir_opt_* passes and their _ex thunks) and 67 KB of .text without a word
# of warning.  --whole-archive reproduces exactly what listing the objects
# individually used to do.  Both GNU ld and tcc's own linker implement it
# (libtcc.c "?whole-archive" -> AFF_WHOLE_ARCHIVE), which matters because the
# YasOS self-host bootstrap relinks this with armv8m-tcc itself.
#
# ARCH_LIB stays outside the group: it is a real library, already linked a la
# carte before this split, and it bundles orphaned members (arm-thumb-scratch.c)
# that must keep being dropped.
MODULE_LIBS = $(DRIVER_LIB) $(FRONTEND_LIB) $(OPT_LIB) $(IR_LIB) \
	$(BACKEND_GENERATORS_LIB) $(MACHINE_LIB) $(OBJ_LIB) $(SUPPORT_LIB) \
	$(MEMORY_LIB)

TCC_LIBS = $(MODULE_LIBS) $(ARCH_LIB)
TCC_FILES = $(DRIVER_MAIN_OBJ) $(TCC_LIBS)
$(X)source/frontend/tccpp.o : $(TCCDEFS_H) tccdefs_table_.h

# Stage B of the predefine pipeline: tccdefs_.h -> tccdefs_table_.h.
#
# Unlike c2str (stage A) this generator must be compiled with the TARGET's
# defines: tccdefs_.h keeps the column-1 conditionals as host directives, so
# "which predefines this target actually has" is only known once the host
# preprocessor has run over it.  Hence $(DEFINES) here -- the same flags the
# cross objects get -- while the binary itself is built for and run on the
# host.  It emits the macros pre-tokenised so the compiler can materialise
# them on demand instead of lexing 3.7 KB of text on every invocation.
#
# Only the -D half of CFLAGS is passed: the target macros are what select the
# right block of tccdefs_.h (without them tcc.h falls back to i386 and the
# build dies on a missing i386-tok.h), while the rest of CFLAGS is ARM codegen
# (-mcpu, -fpie) that a host binary must not be built with.
#
# Deliberately NOT part of $(TCCDEFS_H): that list is also a prerequisite of the
# outer `%-tcc` rule, which runs before $T is set, so DEFINES would carry no
# -DTCC_TARGET_* and tcc.h would select i386.  Hanging it off tccpp.o keeps it
# in the per-target sub-make.  The helper is named per target for the same
# parallel-make reason c2str is (see above).
#
# gcc, not $(CC), for the same reason the c2str rule hardcodes it: this is a
# HOST tool, and in the native/self-host stage $(CC) is armv8m-tcc, which would
# build it for the target (and fail on the yasos libc's missing strtok_r).
tccdefs_table_.h : tccdefs_.h gen_predef_table.c
	$Sgcc -o gen_predef_table-$T$(EXESUF) gen_predef_table.c \
	  $(addsuffix ,$(DEFINES) $(filter -D%,$(CFLAGS))) \
	  && ./gen_predef_table-$T$(EXESUF) $@ && rm -f gen_predef_table-$T$(EXESUF)

DEFINES += -I$(TOP) $(CORE_INC) $(DRIVER_INC) $(FRONTEND_INC) $(IR_INC) \
	$(MACHINE_INC) $(OBJ_INC) $(SUPPORT_INC) -I$(TOP)/source/opt/include \
	$(OPT_DSL_INC) $(SSA_OPT_INC) $(FLAT_OPT_INC) $(MEMORY_INC) $(UTILS_INC) \
	$(GEN_INC) -I$(TOP)/source/backend/arch/arm -I$(TOP)/source/backend/arch/arm/thumb

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
#
# The helper is named per stem. It used to be a single `c2str.exe`, which was
# fine while tccdefs_.h was the only target using this rule; tccdecls_.h made it
# two, and under `make -j` both recipes then wrote and exec'd the same file --
# one of them hitting it mid-link, which fails as "Text file busy" (ETXTBSY) and
# takes the build down with error 126. The cross-tcc rule below already guards
# the same hazard through $(TCCDEFS_H); that guard was never extended to cover a
# second stem.
%_.h : include/%.h conftest.c
	# todo: how to pass host CC there?
	gcc -DC2STR $(filter %.c,$^) -o c2str-$*.exe && ./c2str-$*.exe $< $@ && rm -f c2str-$*.exe

# target specific object rules
# (depend on config.mak so toggling build flags — e.g. ASan via
# ./configure [--disable-asan] — forces a recompile instead of silently
# relinking stale, differently-instrumented objects)
$(X)%.o : %.c $(LIBTCC_INC) config.mak
	@mkdir -p $(dir $@)
	$S$(CC) -o $@ -c $< $(addsuffix ,$(DEFINES) $(CFLAGS))

# Architecture library — built by nested Makefile
TARGET_ARCH_NAME = $($T_ARCH)
ARCH_DEFINES = $(subst -I.,-I$(CURDIR),$(DEFINES))
$(ARCH_LIB): FORCE
	@mkdir -p $(dir $(ARCH_LIB))
	@# Build flags changed (e.g. ASan toggled via configure)?  Drop stale objects
	@# since the nested arch Makefile tracks source and header timestamps, not
	@# flags.  configure's *defines* need no help here: config.h is a header, and
	@# the arch Makefile's CORE_HDRS covers it.
	@if [ -f "$(ARCH_LIB)" ] && [ config.mak -nt "$(ARCH_LIB)" ]; then \
		rm -f $(dir $(ARCH_LIB))*.o "$(ARCH_LIB)"; \
	fi
	$S$(MAKE) --no-print-directory -C source/backend ARCH=$(TARGET_ARCH_NAME) \
		TOP=$(CURDIR) BUILD_DIR=$(CURDIR)/$(dir $(ARCH_LIB)) \
		CC="$(CC)" AR="$(AR)" CFLAGS="$(CFLAGS)" DEFINES="$(ARCH_DEFINES)"

# additional dependencies
# tcctools.c is #included by tcc.c rather than compiled on its own.
$(DRIVER_MAIN_OBJ) : source/driver/tcctools.c
$(DRIVER_MAIN_OBJ) : DEFINES += $(DEF_GITHASH)

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
	$S$(CC) -o $@ $(DRIVER_MAIN_OBJ) -Wl,--whole-archive $(MODULE_LIBS) -Wl,--no-whole-archive $(ARCH_LIB) $(LDFLAGS) $(LIBS)

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
	$(call run-if,perl,$(TOPSRC)/scripts/texi2pod.pl $< $@)

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
	$(call IF,$(TOPSRC)/include/*.h,"$(tccdir)/include")
	@if [ -d "$(TOPSRC)/pch" ]; then echo "-> $(tccdir)/pch : $(TOPSRC)/pch" ; mkdir -p "$(tccdir)/pch" && cp -r "$(TOPSRC)/pch"/. "$(tccdir)/pch" ; fi
	$(call $(if $(findstring .so,$(LIBTCC)),IBw,IFw),$(LIBTCC),"$(libdir)")
	$(call IF,$(TOPSRC)/source/driver/libtcc.h,"$(includedir)")
	$(call IFw,tcc.1,"$(mandir)/man1")
	$(call IFw,tcc-doc.info,"$(infodir)")
	$(call IFw,tcc-doc.html,"$(docdir)")
ifneq "$(wildcard $(LIBTCC1_W))" ""
	$(call IFw,$(TOPSRC)/win32/lib/*.def $(LIBTCC1_W),"$(tccdir)/win32/lib")
	$(call IR,$(TOPSRC)/win32/include,"$(tccdir)/win32/include")
	$(call IF,$(TOPSRC)/include/*.h,"$(tccdir)/win32/include")
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

TAGFILES = $(CORE_FILES) include/*.h lib/*.[chS]
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

# Pytest parallel workers: make test J=16 → pytest -n 16 (default: auto).
# J=1 disables xdist entirely so logs are sequential.
J ?= auto
PYTEST_XDIST ?= -n $(J)
ifeq ($(J),1)
PYTEST_XDIST =
endif

# Verbose pytest output (per-test names) only in CI; keep local runs terse.
# Usage: make test CI=1
CI ?= 0
ifeq ($(CI),1)
PYTEST_VERBOSE := -v
else
PYTEST_VERBOSE :=
endif

# Cross compiler used by pytest test suites.
CROSS_COMPILER = $(CURDIR)/armv8m-tcc

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
PCH_BENCHMARK_SCRIPT := $(IRTESTS_DIR)/benchmark_pch.py
PCH_PREPARE_SCRIPT := $(IRTESTS_DIR)/prepare_pch.py
GOLDEN_IR_COMPILER ?= $(TOP)/armv8m-tcc.debug

NEWLIB_DIR := $(IRTESTS_DIR)/qemu/mps2-an505/newlib_build/arm-none-eabi/newlib
NEWLIB_LIBC_A := $(NEWLIB_DIR)/libc.a

# newlib is a vendored submodule (its include dir is symlinked into
# libc_includes/newlib).  We must not commit edits into it; instead keep local
# fixups as patches under tests/ir_tests/patches and apply them idempotently
# before any target that consumes the headers (warn-check, test-prepare).
NEWLIB_SRC := $(IRTESTS_DIR)/qemu/mps2-an505/libs/newlib
NEWLIB_PATCH_DIR := $(IRTESTS_DIR)/patches

.PHONY: patch-newlib
patch-newlib:
	@for p in $$(ls $(NEWLIB_PATCH_DIR)/*.patch 2>/dev/null | sort); do \
		ap=$$(cd $$(dirname "$$p") && pwd)/$$(basename "$$p"); \
		if git -C $(NEWLIB_SRC) apply --reverse --check "$$ap" >/dev/null 2>&1; then \
			: ; \
		elif git -C $(NEWLIB_SRC) apply --check "$$ap" >/dev/null 2>&1; then \
			echo "------------ newlib: applying patch $$(basename $$p) ------------"; \
			git -C $(NEWLIB_SRC) apply "$$ap"; \
		else \
			echo "WARNING: newlib patch $$(basename $$p) does not apply cleanly (skipping)"; \
		fi; \
	done

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
	@# Bit-exact IEEE-754 conformance against host-generated reference vectors.
	@# Unlike the tests above -- which carry their own copies of the algorithms
	@# -- this compiles and calls the shipped lib/fp/soft sources directly, so a
	@# regression in the library cannot hide behind a re-implementation.
	@echo "Running FP conformance against lib/fp/soft..."
	@CC="$(CC)" tests/fp/run_host_softfp_test.sh || exit 1
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
test-prepare: patch-newlib
	@set -e; \
	if [ -f "$(NEWLIB_LIBC_A)" ]; then exit 0; fi; \
	echo "------------ ir_tests: building newlib (first run) ------------"; \
	cd $(IRTESTS_DIR)/qemu/mps2-an505 && sh ./build_newlib.sh

.PHONY: rebuild-newlib
rebuild-newlib:
	@echo "------------ ir_tests: rebuilding newlib ------------"
	@rm -rf $(IRTESTS_DIR)/qemu/mps2-an505/newlib_build
	@cd $(IRTESTS_DIR)/qemu/mps2-an505 && sh ./build_newlib.sh

.PHONY: prepare-pch benchmark-pch benchmark-pch-libc benchmark-pch-libtcc
prepare-pch: cross
	@$(PYTHON) "$(PCH_PREPARE_SCRIPT)" $(PCH_PREPARE_ARGS)

benchmark-pch: cross
	@$(PYTHON) "$(PCH_BENCHMARK_SCRIPT)" $(PCH_BENCHMARK_ARGS)

benchmark-pch-libc: cross
	@$(PYTHON) "$(PCH_BENCHMARK_SCRIPT)" --scenario libc-common $(PCH_BENCHMARK_ARGS)

benchmark-pch-libtcc: cross
	@$(PYTHON) "$(PCH_BENCHMARK_SCRIPT)" --scenario libtcc $(PCH_BENCHMARK_ARGS)


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
			"$(VENV_PY)" -m pytest --tb=short -q $(PYTEST_XDIST) .; \
		else \
			$(PYTEST) --tb=short -q $(PYTEST_XDIST) .; \
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
warn-check: armv8m-tcc$(EXESUF) patch-newlib
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
	wc_inc="-nostdinc -I$(IRTESTS_DIR)/libc_includes -I$(IRTESTS_DIR)/libc_imports -I$(IRTESTS_DIR)/libc_includes/newlib -Iinclude" ; \
	for f in $(WARN_CHECK_SRCS); do \
		out=$$(./armv8m-tcc$(EXESUF) $$wc_inc -c "$$f" -o /dev/null 2>&1) ; \
		if echo "$$out" | grep -qE 'warning:|error:'; then \
			echo "FAIL: $$f:" ; \
			echo "$$out" | grep -E 'warning:|error:' ; \
			fail=1 ; \
		fi ; \
	done ; \
	if [ "$$fail" -ne 0 ]; then exit 1; fi
	@echo "------------ warn-check: passed ------------"

# -finline-limit must mean the same thing wherever it sits relative to -O2.
# It used to not: -O2 force-RAISED the limit to its own default, so
# `-finline-limit=N -O2` was silently discarded while `-O2 -finline-limit=N`
# worked. The fixture's helper is deliberately sized between the two limits so
# an ignored flag shows up as different code, not just a different size.
.PHONY: optflag-check
optflag-check: armv8m-tcc$(EXESUF)
	@echo "------------ optflag-check: -finline-limit vs -O argument order ------------"
	@d=$$(mktemp -d) ; \
	trap 'rm -rf "$$d"' EXIT ; \
	printf '%s\n' \
		'static int blend(int a, int b, int c) {' \
		'  int t = a * 3 + b * 5;' \
		'  int u = c ^ (a << 2);' \
		'  int v = (t > u) ? (t - u) : (u - t);' \
		'  return v + t - u + (a & b) + (b | c);' \
		'}' \
		'int sink1(int x) { return blend(x, x + 1, x + 2); }' \
		'int sink2(int x) { return blend(x * 2, x, x - 7); }' \
		'int sink3(int x) { return blend(x, 11, x ^ 3); }' \
		> "$$d/ilim.c" ; \
	./armv8m-tcc$(EXESUF) -c "$$d/ilim.c" -O2 -o "$$d/plain.o" || exit 1 ; \
	./armv8m-tcc$(EXESUF) -c "$$d/ilim.c" -O2 -finline-limit=20 -o "$$d/after.o" || exit 1 ; \
	./armv8m-tcc$(EXESUF) -c "$$d/ilim.c" -finline-limit=20 -O2 -o "$$d/before.o" || exit 1 ; \
	if ! cmp -s "$$d/after.o" "$$d/before.o"; then \
		echo "FAIL: -finline-limit=20 -O2 differs from -O2 -finline-limit=20 (argument order changes meaning)" ; \
		exit 1 ; \
	fi ; \
	if cmp -s "$$d/plain.o" "$$d/after.o"; then \
		echo "FAIL: -finline-limit=20 produced the same code as plain -O2 -- the fixture no longer straddles the two limits, so this check proves nothing" ; \
		exit 1 ; \
	fi
	@echo "------------ optflag-check: passed ------------"

# run frontend coverage tests
# Fast, QEMU-free preprocessor / type-system / diagnostic golden tests.
test-frontend: cross
	@echo "------------ frontend tests ------------"
	@if [ "$(USE_VENV)" = "1" ]; then \
		cd $(TOP)/tests/frontend && "$(VENV_PY)" -m pytest -q --compiler=$(CROSS_COMPILER); \
	else \
		cd $(TOP)/tests/frontend && $(PYTEST) -q --compiler=$(CROSS_COMPILER); \
	fi

# run linker/object coverage tests
# Fast, QEMU-free readelf/objdump golden tests.
test-linker: cross
	@echo "------------ linker tests ------------"
	@if [ "$(USE_VENV)" = "1" ]; then \
		cd $(TOP)/tests/linker && "$(VENV_PY)" -m pytest -q; \
	else \
		cd $(TOP)/tests/linker && $(PYTEST) -q; \
	fi

# run debug-info coverage tests
# Fast, QEMU-free DWARF/STAB readelf tests.
test-debug: cross
	@echo "------------ debug-info tests ------------"
	@if [ "$(USE_VENV)" = "1" ]; then \
		cd $(TOP)/tests/debug && "$(VENV_PY)" -m pytest -q; \
	else \
		cd $(TOP)/tests/debug && $(PYTEST) -q; \
	fi

# run runtime-library coverage tests
# Host-native soft-FP tests plus cross-compiled runtime-helper reference tests.
test-runtime: cross
	@echo "------------ runtime-library tests ------------"
	@if [ "$(USE_VENV)" = "1" ]; then \
		cd $(TOP)/tests/runtime && "$(VENV_PY)" -m pytest -q --compiler=$(CROSS_COMPILER); \
	else \
		cd $(TOP)/tests/runtime && $(PYTEST) -q --compiler=$(CROSS_COMPILER); \
	fi

# run self-host bootstrap gate
# Compile-only smoke test always runs; FAT-drive round-trip skips if YasOS env is missing.
test-selfhost: cross
	@echo "------------ self-host bootstrap gate ------------"
	@if [ "$(USE_VENV)" = "1" ]; then \
		cd $(TOP)/tests/selfhost && "$(VENV_PY)" -m pytest -q --compiler=$(CROSS_COMPILER); \
	else \
		cd $(TOP)/tests/selfhost && $(PYTEST) -q --compiler=$(CROSS_COMPILER); \
	fi

# Re-run the floating-point tests under one float ABI.  The ABI is a whole-program
# choice (it selects the matching libc/libm/libgcc/crt), so it is set through the
# harness rather than as a cflag:
#
#   make test-fp FLOAT_ABI=hard      # or softfp / soft (default soft)
#
# `make test` covers the soft ABI plus the self-contained hard-float suite;
# this target is how you exercise libc/libm interop under the other ABIs.
# Note: only single precision is implemented for -mfloat-abi=hard — doubles
# still travel in GPR pairs, so double libm calls are not ABI-compatible yet
# (see docs/plan_vfp_hard_float.md).
FLOAT_ABI ?= soft
.PHONY: test-fp
test-fp: cross test-venv test-prepare
	@echo "------------ float tests (-mfloat-abi=$(FLOAT_ABI)) ------------"
	@if [ "$(USE_VENV)" = "1" ]; then \
		cd $(IRTESTS_DIR) && TCC_FLOAT_ABI=$(FLOAT_ABI) "$(VENV_PY)" -m pytest -s $(PYTEST_XDIST) -k "float or fp_" -m "not golden_ir"; \
	else \
		cd $(IRTESTS_DIR) && TCC_FLOAT_ABI=$(FLOAT_ABI) $(PYTEST) -s $(PYTEST_XDIST) -k "float or fp_" -m "not golden_ir"; \
	fi

# run IR tests via pytest (preferred)
.PHONY: test-ir
test-ir: cross test-venv test-prepare download-gcc-tests
	@echo "------------ ir_tests (pytest) ------------"
	@if [ "$(USE_VENV)" = "1" ]; then \
		cd $(IRTESTS_DIR) && "$(VENV_PY)" -m pytest -s $(PYTEST_VERBOSE) $(PYTEST_XDIST) -m "not golden_ir" --durations=10; \
	else \
		cd $(IRTESTS_DIR) && $(PYTEST) -s $(PYTEST_VERBOSE) $(PYTEST_XDIST) -m "not golden_ir" --durations=10; \
	fi

# container target: runs the full test suite (all test-* targets below)
.NOTPARALLEL: test test-full test-all
test: cross test-aeabi-host test-asm warn-check optflag-check opt-dsl-check test-venv test-prepare download-gcc-tests ut test-frontend test-linker test-debug test-runtime test-selfhost test-ir
	@echo "------------ test suite complete ------------"

# Fully sequential test run: disables pytest-xdist too, for the cleanest logs.
.PHONY: test-sequential
test-sequential:
	@+$(MAKE) --no-print-directory test J=1

# run golden IR snapshot tests explicitly.
# These require a compiler built with CONFIG_TCC_DEBUG because -dump-ir-passes
# is intentionally a debug/diagnostic interface (on by default, see CFLAGS
# above).  `cross` builds that binary first so a plain `make test-golden-ir`
# works from a clean tree; set GOLDEN_IR_COMPILER to point at a different debug
# binary instead.  Filter cases with K=<expr> (passed to pytest -k).
test-golden-ir: cross test-venv
	@echo "------------ golden IR snapshot tests ------------"
	@compiler_arg=""; \
	if [ -x "$(GOLDEN_IR_COMPILER)" ]; then \
		compiler_arg="--compiler $(GOLDEN_IR_COMPILER)"; \
	fi; \
	k_arg=""; \
	if [ -n "$(K)" ]; then \
		k_arg="-k $(K)"; \
	fi; \
	if [ "$(USE_VENV)" = "1" ]; then \
		cd $(IRTESTS_DIR) && "$(VENV_PY)" -m pytest -s $(PYTEST_XDIST) -m "golden_ir" --require-dump-ir $$compiler_arg $$k_arg test_golden_ir.py; \
	else \
		cd $(IRTESTS_DIR) && $(PYTEST) -s $(PYTEST_XDIST) -m "golden_ir" --require-dump-ir $$compiler_arg $$k_arg test_golden_ir.py; \
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
	$S$(TCC) source/driver/tcc.c -o $@ -ftest-coverage $(DEFINES) $(LIBS)

# Merged line-coverage report for source/frontend/gen/ (the former tccgen.c,
# see docs/plan_tccgen_split.md): the real cross compiler with every gen TU
# instrumented, run over the whole compile-test corpus, unioned with the
# isolated tccgen unit tests.  Restores the normal build on exit.  Requires
# lcov/genhtml.  Tunables: COV_JOBS, COV_OLEVELS, COV_OUT, COV_NO_TORTURE=1.
# Output: coverage-tccgen/index.html + coverage-tccgen/tccgen.info
.PHONY: coverage-tccgen
coverage-tccgen:
	@$(TOPSRC)/scripts/coverage_tccgen.py
# test the installed tcc instead
test-install: $(TCCDEFS_H)
	@$(MAKE) -C tests TESTINSTALL=yes #_all

clean:
	@rm -f tcc *-tcc tcc_p tcc_c
	@rm -f tags ETAGS *.o *.a *.so* *.out *.log lib*.def *.exe *.dll
	@rm -rf *-ir/ *-arch/ *-source/
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
		cd $(TOP)/tests && "$(VENV_PY)" run_tests.py --tests2 -v $(PYTEST_XDIST); \
	else \
		cd $(TOP)/tests && $(PYTEST) -v -m tests2 --tb=short $(PYTEST_XDIST) tests/tests2/; \
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
		cd $(IRTESTS_DIR) && "$(VENV_PY)" -m pytest -m "gcc_compile" --tb=short $(PYTEST_XDIST) $$PYTEST_TIMEOUT test_gcc_torture_ir.py; \
	else \
		cd $(IRTESTS_DIR) && $(PYTEST) -m "gcc_compile" --tb=short $(PYTEST_XDIST) $$PYTEST_TIMEOUT test_gcc_torture_ir.py; \
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
		cd $(IRTESTS_DIR) && "$(VENV_PY)" -m pytest -m "gcc_execute" --tb=short $(PYTEST_XDIST) $$PYTEST_TIMEOUT test_gcc_torture_ir.py; \
	else \
		cd $(IRTESTS_DIR) && $(PYTEST) -m "gcc_execute" --tb=short $(PYTEST_XDIST) $$PYTEST_TIMEOUT test_gcc_torture_ir.py; \
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
		cd $(IRTESTS_DIR) && "$(VENV_PY)" -m pytest -m "gcc_torture" --tb=short $(PYTEST_XDIST) $$PYTEST_TIMEOUT test_gcc_torture_ir.py; \
	else \
		cd $(IRTESTS_DIR) && $(PYTEST) -m "gcc_torture" --tb=short $(PYTEST_XDIST) $$PYTEST_TIMEOUT test_gcc_torture_ir.py; \
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

# host-native internal unit tests (see tests/unit/README for the design)
ut:
	$(MAKE) -C tests/unit run

# pipeline pass coverage ledger: compares PASS/PASS_GATED names in
# source/opt/engine/pipeline_table.c + SSA_RUN names against UT_COVERS markers and golden-IR
# directories.  89/89 (100%) reached 2026-07-01 (see docs/plan_ut_next_steps.md);
# --strict now hard-fails on any regression.
check-pass-coverage:
	@python3 tests/unit/check_pass_coverage.py --strict

# gcov line/branch coverage report for the unit tests (requires gcovr).
# Renders HTML + text under tests/unit/<target>/build/coverage/.
# After generating the report, compares files in the coverage report with
# source files in the repository and reports any files not registered in
# coverage measurement.
ut-coverage:
	$(MAKE) -C tests/unit coverage
	@python3 tests/unit/check_coverage_files.py \
		tests/unit/arm/armv8m/build/coverage/coverage.txt \
		$(CURDIR)

ut-clean:
	$(MAKE) -C tests/unit clean

.PHONY: all cross fp-libs clean test test-ir test-sequential test-valgrind test-aeabi-host test-legacy test-tests2 test-gcc-torture test-gcc-torture-compile test-gcc-torture-execute test-full test-all test-frontend test-linker test-debug test-runtime test-selfhost test-golden-ir rebuild-newlib download-gcc-tests tar tags ETAGS doc distclean install uninstall ut ut-coverage ut-clean check-pass-coverage run start_env build_container pull_container push_container container-build container-pull container-push docker-build docker-push docker-start FORCE

# Container image settings. Build/push uses a multi-arch Podman manifest,
# matching the workflow in ../yasos.zig.
CONTAINER_VERSION ?= 0.1.0
CONTAINER_REGISTRY ?= ghcr.io
CONTAINER_REPOSITORY ?= matgla/tinycc-armv8m
CONTAINER_PLATFORMS ?= linux/amd64,linux/arm64
CONTAINER_LOCAL_IMAGE = $(CONTAINER_REPOSITORY)
CONTAINER_REMOTE_IMAGE = $(CONTAINER_REGISTRY)/$(CONTAINER_REPOSITORY)
CONTAINER_LOCAL_VERSION_IMAGE = $(CONTAINER_LOCAL_IMAGE):$(CONTAINER_VERSION)
CONTAINER_REMOTE_VERSION_IMAGE = $(CONTAINER_REMOTE_IMAGE):$(CONTAINER_VERSION)
CONTAINER_REMOTE_LATEST_IMAGE = $(CONTAINER_REMOTE_IMAGE):latest
RUN_CONTAINER ?= ./scripts/run_container.py -v $(CONTAINER_VERSION)

build_container:
	podman manifest rm $(CONTAINER_LOCAL_VERSION_IMAGE) >/dev/null 2>&1 || true
	podman manifest create $(CONTAINER_LOCAL_VERSION_IMAGE)
	podman build --platform $(CONTAINER_PLATFORMS) --manifest $(CONTAINER_LOCAL_VERSION_IMAGE) .

pull_container:
	podman pull $(CONTAINER_REMOTE_VERSION_IMAGE)

push_container: build_container
	podman manifest push --all $(CONTAINER_LOCAL_VERSION_IMAGE) docker://$(CONTAINER_REMOTE_VERSION_IMAGE)
	podman manifest push --all $(CONTAINER_LOCAL_VERSION_IMAGE) docker://$(CONTAINER_REMOTE_LATEST_IMAGE)

run: pull_container
	$(RUN_CONTAINER) $(if $(CMD),-c "$(CMD)",-i)

start_env: run

# Hyphenated and docker-* aliases for backwards compatibility.
container-build: build_container
container-pull: pull_container
container-push: push_container
docker-build: build_container
docker-push: push_container
docker-start: run

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
	@echo "   run the full test suite (test-ir + test-asm + warn-check + ut + ...)"
	@echo "make test-ir"
	@echo "   rebuild + initialize GCC testsuite + run pytest in tests/ir_tests"
	@echo "make test-sequential"
	@echo "   same as make test, but runs pytest sequentially for clean logs"
	@echo "make rebuild-newlib"
	@echo "   wipe and rebuild newlib used by ir_tests/qemu (mps2-an505)"
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
	@echo "   build_container"
	@echo "      build $(CONTAINER_PLATFORMS) manifest $(CONTAINER_LOCAL_VERSION_IMAGE)"
	@echo "   pull_container"
	@echo "      pull $(CONTAINER_REMOTE_VERSION_IMAGE)"
	@echo "   push_container"
	@echo "      build and push $(CONTAINER_REMOTE_VERSION_IMAGE) and $(CONTAINER_REMOTE_LATEST_IMAGE)"
	@echo "   run"
	@echo "      start container shell with repo and persistent history mounted"
	@echo "   run CMD='make test'"
	@echo "      run a command inside the container"
	@echo "   container-build/container-pull/container-push (legacy aliases)"
	@echo "   docker-build (legacy alias)"
	@echo "   docker-push (legacy alias)"
	@echo "   docker-start/start_env (legacy aliases for run)"
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
