# RP2350 Double Coprocessor Library Makefile
# Uses DCP (Double Coprocessor) for double-precision floating point
# Single-precision uses VFPv4 or software fallback

include ../../../../config.mak

SRCS = dcp_init.c dcp_ops.c dcp_cmp.c dcp_conv.c
BUILD_DIR ?= build
OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o))

CFLAGS += -O2 -Wall -Wextra -march=armv7e-m -DRP2350_DCP_ENABLED -I../../..

all: $(BUILD_DIR) $(OBJS)

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
