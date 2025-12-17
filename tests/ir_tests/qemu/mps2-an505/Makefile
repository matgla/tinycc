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

CFLAGS += -nostdlib -g -fvisibility=hidden -mcpu=cortex-m33
LDFLAGS = $(CFLAGS) -g -fvisibility=hidden

ifneq (,$(findstring armv8m-tcc,$(CC)))
CFLAGS += -I$(MAKEFILE_DIR)/../../libc_includes -I$(ARM_SYSROOT)/include -I$(MAKEFILE_DIR)../../../../include
LDFLAGS += -L$(MAKEFILE_DIR)/../../../.. -L$(SYSROOT) -llibc.a -llibrdimon.a -larmv8m-libtcc1.a -Wl,-oformat=elf32-littlearm -Wl,-Ttext=0x10000000
else
LDFLAGS += -Wl,--start-group -lrdimon -lc_nano -lgcc -Wl,--end-group --specs=rdimon.specs -T$(MAKEFILE_DIR)linker_script.ld
endif


SRCS = $(MAKEFILE_DIR)boot.c $(TEST_FILES)
ASMS =
OBJS = $(patsubst $(MAKEFILE_DIR)%.c, $(OUTPUT)/%.o, $(SRCS))
OBJS += $(patsubst $(MAKEFILE_DIR)%.s, $(OUTPUT)/%.o, $(ASMS))

CRT_LIBS =  $(CRT_GCC)/crti.o $(CRT_GCC)/crtbegin.o $(SYSROOT)/rdimon-crt0.o $(CRT_GCC)/crtend.o $(CRT_GCC)/crtn.o

# Rules
all: $(TARGET)
	@echo "Build complete: $(TARGET)"

$(OUTPUT)/%.o: $(MAKEFILE_DIR)%.c
	mkdir -p $(OUTPUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUTPUT)/%.o: $(MAKEFILE_DIR)%.s
	mkdir -p $(OUTPUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $^ $(CRT_LIBS) -o $@ $(LDFLAGS)

clean:
	rm -rf $(OUTPUT)