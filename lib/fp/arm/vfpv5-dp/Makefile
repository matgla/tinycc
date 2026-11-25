# ARM VFPv5 Double-Precision Library Makefile
# Optimized for Cortex-M7 (full double-precision hardware FPU)
# Both single and double precision use hardware

include ../../../../config.mak

FP_CC ?= $(CC)
FP_CFLAGS ?= $(CFLAGS)

SRCS = ops.c cmp.c conv.c
# complex division (__divsc3/__divdc3), as in every variant; here its
# arithmetic is the hardware's (softfp: base-PCS arguments, inline VFP)
SOFT_DIR  = ../../soft
SOFT_SRCS = divc3.c
BUILD_DIR ?= build
OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o)) $(addprefix $(BUILD_DIR)/,$(SOFT_SRCS:.c=.o))

override FP_CFLAGS += -O2 -Wall -Wextra -march=armv7e-m -mfpu=fpv5-d16 -I../../.. -I../../../../include

all: $(BUILD_DIR) $(OBJS)

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: %.c
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SOFT_DIR)/%.c
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
