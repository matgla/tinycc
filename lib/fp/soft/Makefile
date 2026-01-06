# Soft Floating Point Library Makefile
# Builds software implementations of ARM EABI floating point functions

include ../../../config.mak

SRCS = fadd.c fmul.c fdiv.c fcmp.c dadd.c dmul.c ddiv.c dconv.c dcmp.c conv.c fmt.c
BUILD_DIR ?= build
OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o))

CFLAGS += -O2 -Wall -Wextra -I../.. -I../../../include

all: $(BUILD_DIR) $(OBJS)

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
