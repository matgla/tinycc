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

GCC_ABI_FLAGS = -mcpu=cortex-m33 -mthumb -mfloat-abi=soft #-mfloat-abi=hard -mfpu=fpv5-sp-d16

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
# Note: newlib_build in this repo is a single-variant build (no multilib).
USE_NEWLIB_BUILD ?= 1

# If set to 1, link against debug libc/libm (libc_g.a, libm_g.a) when available.
DEBUG_LIBC ?= 1

NEWLIB_BUILD_DIR = $(MAKEFILE_DIR)/newlib_build
NEWLIB_DIR = $(NEWLIB_BUILD_DIR)/arm-none-eabi/newlib
NEWLIB_LIBC_G = $(NEWLIB_DIR)/libc_g.a
NEWLIB_LIBM_G = $(NEWLIB_DIR)/libm_g.a

EXTRA_CFLAGS ?=
CFLAGS += -nostdlib -g -fvisibility=hidden $(GCC_ABI_FLAGS) -gdwarf -ffunction-sections $(EXTRA_CFLAGS)
TCC_PATH = $(shell realpath $(MAKEFILE_DIR)../../../../)
LDFLAGS = -Wl,--gc-sections

LIBC_INCLUDES = $(shell realpath $(MAKEFILE_DIR)../../libc_includes)
ifeq ($(USE_NEWLIB_BUILD),1)
LIBGLOSS_PATH = $(shell realpath $(NEWLIB_BUILD_DIR)/arm-none-eabi/libgloss/arm)
else
LIBGLOSS_PATH = $(shell dirname $(RDIMON_CRT0_PATH))
endif

CRT_LIBS =

ifneq (,$(findstring armv8m-tcc,$(CC)))
CFLAGS += -I$(LIBC_INCLUDES) -I$(ARM_SYSROOT)/include -I$(TCC_PATH)/include
LDFLAGS += -B$(TCC_PATH)
ifeq ($(USE_NEWLIB_BUILD),1)
LIBC_LIB := $(if $(and $(filter 1,$(DEBUG_LIBC)),$(wildcard $(NEWLIB_LIBC_G))),c_g,c)
LIBM_LIB := $(if $(and $(filter 1,$(DEBUG_LIBC)),$(wildcard $(NEWLIB_LIBM_G))),m_g,m)
LDFLAGS += -L$(TCC_PATH)/lib/fp -L$(TCC_PATH) -L$(NEWLIB_DIR) -L$(LIBGLOSS_PATH) -Wl,--start-group -larmv8m-libtcc1.a -llibtcc1-fp-soft-armv8m.a -l$(LIBC_LIB) -llibrdimon.a -l$(LIBM_LIB) $(LIBGCC_PATH) -Wl,--end-group -Wl,-oformat=elf32-littlearm -T$(MAKEFILE_DIR)linker_script.ld -v
CRT_LIBS = $(CRT_CRTI_PATH) $(LIBGLOSS_PATH)/rdimon-crt0.o $(CRT_CRTEND_PATH) $(CRT_CRTN_PATH)
else
LIBC_A_PATH := $(if $(filter 1,$(DEBUG_LIBC)),$(LIBC_G_PATH),$(LIBC_PATH))
LIBM_A_PATH := $(if $(filter 1,$(DEBUG_LIBC)),$(LIBM_G_PATH),$(LIBM_PATH))
LDFLAGS += -L$(TCC_PATH)/lib/fp -L$(TCC_PATH) -Wl,--start-group $(LIBC_A_PATH) $(LIBRDIMON_PATH) -llibtcc1-fp-soft-armv8m.a -larmv8m-libtcc1.a $(LIBM_A_PATH) $(LIBC_A_PATH) $(LIBGCC_PATH) -Wl,--end-group -Wl,-oformat=elf32-littlearm -T$(MAKEFILE_DIR)linker_script.ld -v
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
	$(CC_WRAPPER) $(CC) $(CFLAGS) -c $< -o $@

$(OUTPUT)/%.o: $(MAKEFILE_DIR)%.S | $(OUTPUT)
	$(CC_WRAPPER) $(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC_WRAPPER) $(CC) $(CFLAGS) $^ $(CRT_LIBS) -o $@ $(LDFLAGS)

# Report binary size after build
size: $(TARGET)
	@$(SIZE_CMD) $(TARGET)

clean:
	rm -rf $(OUTPUT)
	@rm -f $(NEWLIB_BUILD_DIR)/.built

.PHONY: all clean size newlib