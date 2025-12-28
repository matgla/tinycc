# Get the directory where this Makefile is located
MAKEFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

CC ?= $(MAKEFILE_DIR)../../../../armv8m-tcc
TEST_FILES ?= $(MAKEFILE_DIR)main.c
OUTPUT ?= $(MAKEFILE_DIR)build/
TARGET ?= $(OUTPUT)/hello.elf

ARM_SYSROOT = $(shell arm-none-eabi-gcc --print-sysroot)
SYSROOT = $(shell realpath $(ARM_SYSROOT)/lib/thumb/v8-m.base/nofp)

LIBGCC_PATH = $(shell arm-none-eabi-gcc -print-libgcc-file-name)
CRT_GCC = $(dir $(LIBGCC_PATH))/thumb/v8-m.base/nofp

CFLAGS += -nostdlib -g -fvisibility=hidden -mcpu=cortex-m33 -gdwarf -mfloat-abi=hard -mfpu=fpv5-sp-d16 -ffunction-sections
LDFLAGS = $(CFLAGS) -g -fvisibility=hidden -Wl,--gc-sections

TCC_PATH = $(shell realpath $(MAKEFILE_DIR)../../../../)
LIBC_INCLUDES = $(shell realpath $(MAKEFILE_DIR)../../libc_includes)
LIBGLOSS_PATH = $(shell realpath $(MAKEFILE_DIR)/newlib_build/arm-none-eabi/libgloss/arm)

ifneq (,$(findstring armv8m-tcc,$(CC)))
CFLAGS += -I$(LIBC_INCLUDES) -I$(ARM_SYSROOT)/include -I$(TCC_PATH)/include
LDFLAGS += -L$(TCC_PATH) -L$(MAKEFILE_DIR)/newlib_build/arm-none-eabi/newlib -L$(LIBGLOSS_PATH) -llibc.a -llibrdimon.a -larmv8m-libtcc1.a -llibm.a -llibc.a -Wl,-oformat=elf32-littlearm -T$(MAKEFILE_DIR)linker_script.ld -v
else
LDFLAGS += -Wl,--start-group -lrdimon -lc -lgcc -Wl,--end-group --specs=rdimon.specs -T$(MAKEFILE_DIR)linker_script.ld
endif


SRCS = $(TEST_FILES)
ASMS = $(MAKEFILE_DIR)boot.S
OBJS = $(patsubst $(MAKEFILE_DIR)%.c, $(OUTPUT)/%.o, $(SRCS))
OBJS += $(patsubst $(MAKEFILE_DIR)%.S, $(OUTPUT)/%.o, $(ASMS))

CRT_LIBS = $(CRT_GCC)/crti.o $(LIBGLOSS_PATH)/rdimon-crt0.o $(CRT_GCC)/crtend.o $(CRT_GCC)/crtn.o

# Rules
all: $(TARGET)
	@echo "Build complete: $(TARGET)"

$(OUTPUT):
	mkdir -p $(OUTPUT)

$(OUTPUT)/%.o: $(MAKEFILE_DIR)%.c | $(OUTPUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUTPUT)/%.o: $(MAKEFILE_DIR)%.S | $(OUTPUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $^ $(CRT_LIBS) -o $@ $(LDFLAGS)

clean:
	rm -rf $(OUTPUT)