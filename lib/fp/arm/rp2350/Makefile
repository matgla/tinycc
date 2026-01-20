# RP2350 Double Coprocessor Library Makefile
# Uses DCP (Double Coprocessor) for double-precision floating point
# Single-precision uses VFPv4 or software fallback

include ../../../../config.mak

FP_CC ?= $(CC)
FP_CFLAGS ?= $(CFLAGS)

SRCS = dcp_init.c dcp_ops.c dcp_cmp.c dcp_conv.c
BUILD_DIR ?= build
OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o))

FP_CFLAGS += -O2 -Wall -Wextra -march=armv7e-m -DRP2350_DCP_ENABLED -I../../.. -I../../../../include

all: $(BUILD_DIR) $(OBJS)

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: %.c
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
