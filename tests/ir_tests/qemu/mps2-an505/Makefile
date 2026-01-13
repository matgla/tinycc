# Get the directory where this Makefile is located
MAKEFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

CC ?= $(MAKEFILE_DIR)../../../../armv8m-tcc
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

# Toolchain multilib runtime (matches GCC_ABI_FLAGS)
RDIMON_CRT0_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=rdimon-crt0.o)
LIBC_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=libc.a)
LIBM_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=libm.a)
LIBRDIMON_PATH = $(shell arm-none-eabi-gcc $(GCC_ABI_FLAGS) -print-file-name=librdimon.a)

# If set to 1, force using the locally built newlib/libgloss archives.
# Note: newlib_build in this repo is a single-variant build (no multilib).
USE_NEWLIB_BUILD ?= 0

EXTRA_CFLAGS ?=
CFLAGS += -nostdlib -g -fvisibility=hidden $(GCC_ABI_FLAGS) -gdwarf -ffunction-sections $(EXTRA_CFLAGS)
LDFLAGS = $(CFLAGS) -g -fvisibility=hidden -B$(TCC_PATH) -Wl,--gc-sections

TCC_PATH = $(shell realpath $(MAKEFILE_DIR)../../../../)
LIBC_INCLUDES = $(shell realpath $(MAKEFILE_DIR)../../libc_includes)
LIBGLOSS_PATH = $(shell realpath $(MAKEFILE_DIR)/newlib_build/arm-none-eabi/libgloss/arm)
ifneq (,$(findstring armv8m-tcc,$(CC)))
CFLAGS += -I$(LIBC_INCLUDES) -I$(ARM_SYSROOT)/include -I$(TCC_PATH)/include
ifeq ($(USE_NEWLIB_BUILD),1)
LDFLAGS += -L$(TCC_PATH)/lib/fp -L$(TCC_PATH) -L$(MAKEFILE_DIR)/newlib_build/arm-none-eabi/newlib -L$(LIBGLOSS_PATH) -larmv8m-libtcc1.a  -llibtcc1-fp-soft-armv8m.a -llibc.a -llibrdimon.a -llibm.a -Wl,-oformat=elf32-littlearm -T$(MAKEFILE_DIR)linker_script.ld -v
else
LDFLAGS += -L$(TCC_PATH)/lib/fp -L$(TCC_PATH) $(LIBC_PATH) $(LIBRDIMON_PATH) -llibtcc1-fp-soft-armv8m.a -larmv8m-libtcc1.a $(LIBM_PATH) $(LIBC_PATH) -Wl,-oformat=elf32-littlearm -T$(MAKEFILE_DIR)linker_script.ld -v
endif
else
LDFLAGS += -Wl,--start-group -lrdimon -lc -lgcc -Wl,--end-group --specs=rdimon.specs -T$(MAKEFILE_DIR)linker_script.ld
endif


SRCS = $(TEST_FILES)
ASMS = $(MAKEFILE_DIR)boot.S
OBJS = $(patsubst $(MAKEFILE_DIR)%.c, $(OUTPUT)/%.o, $(SRCS))
OBJS += $(patsubst $(MAKEFILE_DIR)%.S, $(OUTPUT)/%.o, $(ASMS))

ifeq ($(USE_NEWLIB_BUILD),1)
CRT_LIBS = $(CRT_GCC)/crti.o $(LIBGLOSS_PATH)/rdimon-crt0.o $(CRT_GCC)/crtend.o $(CRT_GCC)/crtn.o
else
CRT_LIBS = $(CRT_GCC)/crti.o $(RDIMON_CRT0_PATH) $(CRT_GCC)/crtend.o $(CRT_GCC)/crtn.o
endif

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
	$(CC_WRAPPER) $(CC) $^ $(CRT_LIBS) -o $@ $(LDFLAGS)

# Report binary size after build
size: $(TARGET)
	@$(SIZE_CMD) $(TARGET)

clean:
	rm -rf $(OUTPUT)

.PHONY: all clean size