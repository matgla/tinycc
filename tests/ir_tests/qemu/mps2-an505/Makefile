# Get the directory where this Makefile is located
MAKEFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

CC ?= $(MAKEFILE_DIR)../../../../armv8m-tcc
NEWLIB_CC ?= arm-none-eabi-gcc
TEST_FILES ?= $(MAKEFILE_DIR)main.c
OUTPUT ?= $(MAKEFILE_DIR)build/
TARGET ?= $(OUTPUT)/hello.elf

# Profiling support
# CC_WRAPPER: prefix for compiler invocations (e.g., "heaptrack --record-only -o /tmp/ht" or "/usr/bin/time -v -a -o /tmp/metrics.txt")
# Set to empty by default (no profiling)
CC_WRAPPER ?=

# Size tool for binary analysis
SIZE_CMD ?= arm-none-eabi-size

# Float ABI under test:
#   soft   - no FP instructions at all, FP arguments in GPRs
#   softfp - FP instructions allowed, FP arguments still in GPRs
#   hard   - AAPCS-VFP: FP arguments/results in VFP registers (s0-s15)
# The ABI must match the libraries we link against, or float arguments are
# passed one way and read another.  FLOAT_ABI drives both the compiler flags and
# the library selection below, so `make ... FLOAT_ABI=hard` is self-consistent.
FLOAT_ABI ?= soft
FPU ?= fpv5-sp-d16

ifeq ($(FLOAT_ABI),soft)
FLOAT_FLAGS = -mfloat-abi=soft
else
FLOAT_FLAGS = -mfloat-abi=$(FLOAT_ABI) -mfpu=$(FPU)
endif

GCC_ABI_FLAGS = -mcpu=cortex-m33 -mthumb $(FLOAT_FLAGS)

ARM_SYSROOT = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) --print-sysroot)
MULTI_DIR = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-multi-directory)

LIBGCC_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-libgcc-file-name)
CRT_GCC = $(dir $(LIBGCC_PATH))
ifeq ($(USE_NEWLIB_BUILD),1)
CRT_CRTI_PATH = $(shell $(NEWLIB_CC) $(GCC_ABI_FLAGS) -print-file-name=crti.o)
CRT_CRTEND_PATH = $(shell $(NEWLIB_CC) $(GCC_ABI_FLAGS) -print-file-name=crtend.o)
CRT_CRTN_PATH = $(shell $(NEWLIB_CC) $(GCC_ABI_FLAGS) -print-file-name=crtn.o)
else
CRT_CRTI_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=crti.o)
CRT_CRTEND_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=crtend.o)
CRT_CRTN_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=crtn.o)
endif

# Toolchain multilib runtime (matches GCC_ABI_FLAGS)
RDIMON_CRT0_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=rdimon-crt0.o)
LIBC_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=libc.a)
LIBM_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=libm.a)
LIBC_G_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=libc_g.a)
LIBM_G_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=libm_g.a)
LIBRDIMON_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=librdimon.a)

# If set to 1, force using the locally built newlib/libgloss archives.
# Note: each newlib_build dir is a single-variant build (no multilib), so there
# is one per float ABI.  Only the soft one is built by default; for the other
# ABIs the toolchain ships a complete matching multilib
# (thumb/v8-m.main+fp/{softfp,hard}: libc, libm, libgcc, librdimon, crt), so we
# use that instead of requiring a local newlib build per ABI.  Override with
# USE_NEWLIB_BUILD=1 after running `sh build_newlib.sh $(FLOAT_ABI)`.
ifeq ($(FLOAT_ABI),soft)
USE_NEWLIB_BUILD ?= 1
else
USE_NEWLIB_BUILD ?= 0
endif

# If set to 1, link against debug libc/libm (libc_g.a, libm_g.a) when available.
DEBUG_LIBC ?= 1

# One newlib build tree per float ABI; the soft one keeps the historical name.
ifeq ($(FLOAT_ABI),soft)
NEWLIB_BUILD_DIR = $(MAKEFILE_DIR)/newlib_build
else
NEWLIB_BUILD_DIR = $(MAKEFILE_DIR)/newlib_build-$(FLOAT_ABI)
endif
NEWLIB_DIR = $(NEWLIB_BUILD_DIR)/arm-none-eabi/newlib
NEWLIB_LIBC_G = $(NEWLIB_DIR)/libc_g.a
NEWLIB_LIBM_G = $(NEWLIB_DIR)/libm_g.a

EXTRA_CFLAGS ?=
# Prebuilt objects linked into the test (e.g. gcc-compiled, for ABI interop).
EXTRA_OBJS ?=
CFLAGS += -nostdlib -fvisibility=hidden $(GCC_ABI_FLAGS) -ffunction-sections $(EXTRA_CFLAGS)

# Two-phase compilation support (reduces memory usage)
TWO_PHASE ?=
TCC_FLAGS +=
ifneq ($(TWO_PHASE),)
TCC_FLAGS += -two-phase
endif
TCC_PATH = $(shell realpath $(MAKEFILE_DIR)../../../../)
LDFLAGS = -Wl,--gc-sections

LIBC_INCLUDES = $(shell realpath $(MAKEFILE_DIR)../../libc_includes)
LIBC_IMPORTS = $(shell realpath $(MAKEFILE_DIR)../../libc_imports)
NEWLIB_INCLUDES = $(LIBC_INCLUDES)/newlib
ifeq ($(USE_NEWLIB_BUILD),1)
LIBGLOSS_PATH = $(abspath $(NEWLIB_BUILD_DIR)/arm-none-eabi/libgloss/arm)
else
LIBGLOSS_PATH = $(shell dirname $(RDIMON_CRT0_PATH))
endif

CRT_LIBS =
LIBTCC1_BM_A :=
LDFLAGS_TAIL :=

# The libtcc1-bm rule below sits before `all`, so pin the default goal.
.DEFAULT_GOAL := all

ifneq (,$(findstring armv8m-tcc,$(CC)))
CFLAGS += -I$(LIBC_INCLUDES) -I$(LIBC_IMPORTS) -I$(NEWLIB_INCLUDES) -I$(ARM_SYSROOT)/include -I$(TCC_PATH)/include
TCC_FLAGS += -B$(TCC_PATH)
# These images are bare metal: nothing sets up a GOT base in R9, so the
# SB-relative addressing a YASOS build defaults to has no runtime to back it.
# -mno-sb-relative-got alone is not enough since text_and_data_separation
# (on by default for the YASOS target) also addresses globals R9-relative;
# -fno-pic turns the whole mode off, which is the codegen an "ARM Linux"
# cross produces anyway -- i.e. what these tests always exercised.  R9-
# relative coverage lives in the yasos.zig QEMU smoke, which has the loader.
TCC_FLAGS += -fno-pic -mno-sb-relative-got

# The tree's armv8m-libtcc1.a has the same problem INSIDE it: it is built
# YasOS-flavoured, so builtin.o reads its global through the R9 GOT base and
# any helper use faults on bare metal no matter what flags this side passes
# (358_ssa_clrsb_fold: HardFault at the helper's `ldr Rt,[r9,#imm]`).  Build
# the same members bare-metal-flavoured, once per compiler binary, and shadow
# the shared archive: -L first so the explicit -larmv8m-libtcc1.a resolves
# here, and -B LAST so the linker runtime's unconditional libtcc1 pull
# (tcc_add_support; the last -B owns the {B} library paths) also lands here.
LIBTCC1_BM_MEMBERS = libtcc1 alloca armeabi armeabi_divmod va_list builtin arm_string arm_mem arm_copystub armv8m_atomic
LIBTCC1_BM_CC_SUM := $(shell sha256sum "$(CC)" 2>/dev/null | cut -c1-12)
ifeq ($(strip $(LIBTCC1_BM_CC_SUM)),)
LIBTCC1_BM_CC_SUM := unknowncc
endif
LIBTCC1_BM_DIR := $(MAKEFILE_DIR)libtcc1-bm/$(LIBTCC1_BM_CC_SUM)/
LIBTCC1_BM_A := $(LIBTCC1_BM_DIR)armv8m-libtcc1.a
$(LIBTCC1_BM_A): $(wildcard $(TCC_PATH)/lib/*.c) $(wildcard $(TCC_PATH)/lib/*.S) $(wildcard $(TCC_PATH)/lib/*.h)
	@mkdir -p $(LIBTCC1_BM_DIR)obj
	@for f in $(LIBTCC1_BM_MEMBERS); do \
	  src=$(TCC_PATH)/lib/$$f.c; \
	  [ -f "$$src" ] || src=$(TCC_PATH)/lib/$$f.S; \
	  $(CC_WRAPPER) $(CC) -fno-pic -mno-sb-relative-got -I$(TCC_PATH)/include -I$(TCC_PATH)/lib \
	    -c $$src -o $(LIBTCC1_BM_DIR)obj/$$f.o || exit 1; \
	done
	ar rcs $@ $(addprefix $(LIBTCC1_BM_DIR)obj/,$(addsuffix .o,$(LIBTCC1_BM_MEMBERS)))
LDFLAGS := -L$(LIBTCC1_BM_DIR) $(LDFLAGS)
LDFLAGS += -B$(TCC_PATH)
# The shadowing -B: appended at the link command's very end so it is the
# LAST -B (the one that owns the {B} library paths) -- the branch lines
# below append more LDFLAGS after this.
LDFLAGS_TAIL := -B$(LIBTCC1_BM_DIR)

# `-lsoftfp` resolves to lib/fp/libsoftfp.so (the yaff shared object) before
# libsoftfp.a, which left every __aeabi_* symbol to be supplied by libgcc --
# so the FP suite was silently testing libgcc, not the in-tree soft-float
# library, and 524 conformance vectors' worth of bugs stayed invisible.
# Name the static archive explicitly.  Defined outside the USE_NEWLIB_BUILD
# split below because both branches reference it.
SOFTFP_A := $(TCC_PATH)/lib/fp/libsoftfp.a

ifeq ($(USE_NEWLIB_BUILD),1)
NEWLIB_LIBC_A := $(if $(and $(filter 1,$(DEBUG_LIBC)),$(wildcard $(NEWLIB_LIBC_G))),$(NEWLIB_LIBC_G),$(NEWLIB_DIR)/libc.a)
NEWLIB_LIBM_A := $(if $(and $(filter 1,$(DEBUG_LIBC)),$(wildcard $(NEWLIB_LIBM_G))),$(NEWLIB_LIBM_G),$(NEWLIB_DIR)/libm.a)
NEWLIB_LIBRDIMON_A := $(LIBGLOSS_PATH)/librdimon.a
LDFLAGS += -L$(TCC_PATH)/lib -L$(TCC_PATH)/lib/fp -L$(TCC_PATH) -Wl,--start-group -larmv8m-libtcc1.a $(SOFTFP_A) $(NEWLIB_LIBC_A) $(NEWLIB_LIBRDIMON_A) $(NEWLIB_LIBM_A) $(LIBGCC_PATH) -Wl,--end-group -Wl,-oformat=elf32-littlearm -T$(MAKEFILE_DIR)linker_script.ld -v
CRT_LIBS = $(CRT_CRTI_PATH) $(LIBGLOSS_PATH)/rdimon-crt0.o $(CRT_CRTEND_PATH) $(CRT_CRTN_PATH)
else
# The toolchain multilibs ship no debug (libc_g.a / libm_g.a) variants — only
# the locally built newlib does — so fall back to the normal archives when the
# debug ones are absent, as the USE_NEWLIB_BUILD=1 branch above already does.
LIBC_A_PATH := $(if $(and $(filter 1,$(DEBUG_LIBC)),$(wildcard $(LIBC_G_PATH))),$(LIBC_G_PATH),$(LIBC_PATH))
LIBM_A_PATH := $(if $(and $(filter 1,$(DEBUG_LIBC)),$(wildcard $(LIBM_G_PATH))),$(LIBM_G_PATH),$(LIBM_PATH))
# libm precedes libtcc1: lib/builtin.c carries freestanding fallbacks for C
# float APIs (fabsf, floorf, fmaxf, ...) built with the compiler's own default
# ABI, so with a real libm present those must not shadow it — under
# -mfloat-abi=hard the fallbacks would read float arguments from GPRs while the
# caller passed them in s0-s15.  The C library owns the C library functions;
# libtcc1 only supplies compiler runtime helpers.
LDFLAGS += -L$(TCC_PATH)/lib -L$(TCC_PATH)/lib/fp -L$(TCC_PATH) -Wl,--start-group $(LIBC_A_PATH) $(LIBRDIMON_PATH) $(SOFTFP_A) $(LIBM_A_PATH) -larmv8m-libtcc1.a $(LIBC_A_PATH) $(LIBGCC_PATH) -Wl,--end-group -Wl,-oformat=elf32-littlearm -T$(MAKEFILE_DIR)linker_script.ld -v
CRT_LIBS = $(CRT_CRTI_PATH) $(RDIMON_CRT0_PATH) $(CRT_CRTEND_PATH) $(CRT_CRTN_PATH)
endif
else
LDFLAGS += -Wl,--start-group -lrdimon -lc -lgcc -Wl,--end-group --specs=rdimon.specs -T$(MAKEFILE_DIR)linker_script.ld
endif

SRCS = $(TEST_FILES)
ASMS = $(MAKEFILE_DIR)boot.S
OBJS = $(patsubst $(MAKEFILE_DIR)%.c, $(OUTPUT)/%.o, $(SRCS))
OBJS += $(patsubst $(MAKEFILE_DIR)%.S, $(OUTPUT)/%.o, $(ASMS))

# Rules
all: $(TARGET)
	@echo "Build complete: $(TARGET)"

$(OUTPUT):
	mkdir -p $(OUTPUT)

$(OUTPUT)/%.o: $(MAKEFILE_DIR)%.c | $(OUTPUT)
	$(CC_WRAPPER) $(CC) $(CFLAGS) $(TCC_FLAGS) -c $< -o $@

$(OUTPUT)/%.o: $(MAKEFILE_DIR)%.S | $(OUTPUT)
	$(CC_WRAPPER) $(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(LIBTCC1_BM_A) $(if $(filter 1,$(USE_NEWLIB_BUILD)),$(NEWLIB_DIR)/libc.a)
	$(CC_WRAPPER) $(CC) $(CFLAGS) $(TCC_FLAGS) $(OBJS) $(EXTRA_OBJS) $(CRT_LIBS) -o $@ $(LDFLAGS) $(LDFLAGS_TAIL)

# Build newlib from source if not already built (soft ABI by default; other
# ABIs normally use the toolchain multilib instead — see USE_NEWLIB_BUILD).
$(NEWLIB_DIR)/libc.a:
	@echo "newlib ($(FLOAT_ABI)) not found, building..."
	cd $(MAKEFILE_DIR) && sh build_newlib.sh $(FLOAT_ABI) $(FPU)

# Report binary size after build
size: $(TARGET)
	@$(SIZE_CMD) $(TARGET)

clean:
	rm -rf $(OUTPUT)
	@rm -f $(NEWLIB_BUILD_DIR)/.built

.PHONY: all clean size newlib
