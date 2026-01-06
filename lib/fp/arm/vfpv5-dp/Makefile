# ARM VFPv5 Double-Precision Library Makefile
# Optimized for Cortex-M7 (full double-precision hardware FPU)
# Both single and double precision use hardware

include ../../../../config.mak

SRCS = ops.c cmp.c conv.c
BUILD_DIR ?= build
OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o))

CFLAGS += -O2 -Wall -Wextra -march=armv7e-m -mfpu=fpv5-d16 -I../../.. -I../../../../include

all: $(BUILD_DIR) $(OBJS)

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
