# ARM VFPv4 Single-Precision Library Makefile
# Optimized for Cortex-M4F (single-precision hardware FPU)
# Double-precision operations delegated to software

include ../../../../config.mak

FP_CC ?= $(CC)
FP_CFLAGS ?= $(CFLAGS)

# Single-precision hardware operations
SRCS_SP = fops.c fcmp.c conv.c
# Double-precision software fallback
SRCS_DP = dops_soft.c

SRCS = $(SRCS_SP) $(SRCS_DP)
BUILD_DIR ?= build
OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o))

override FP_CFLAGS += -O2 -Wall -Wextra -march=armv7e-m -mfpu=fpv4-sp-d16 -I../../.. -I../../../../include

all: $(BUILD_DIR) $(OBJS)

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: %.c
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
