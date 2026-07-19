# Soft Floating Point Library Makefile
# Builds software implementations of ARM EABI floating point functions

include ../../../config.mak

FP_CC ?= $(CC)
FP_CFLAGS ?= $(CFLAGS)

SRCS = fadd.c fmul.c fdiv.c fcmp.c dadd.c dmul.c ddiv.c dconv.c dcmp.c conv.c conv64.c f2d_stub.S fcmp_asm.S dcmp_asm.S
BUILD_DIR ?= build
OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o))
OBJS := $(OBJS:.S=.o)

override FP_CFLAGS += -O2 -Wall -Wextra -I../.. -I../../../include

# Architecture flags for ARM cross-compilation
ARCH_FLAGS = -mcpu=cortex-m33 -mthumb
override FP_CFLAGS += $(ARCH_FLAGS)

all: $(BUILD_DIR) $(OBJS)

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: %.c
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
