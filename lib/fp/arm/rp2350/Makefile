# RP2350 floating-point runtime.
#
# Doubles go through the DCP (Raspberry Pi's double coprocessor on CP4);
# single precision uses the Cortex-M33's FPv5-SP FPU.  Both live in
# dcp_aeabi.S -- see the header comment there for why they are assembly.
#
# Division, square root and the 64-bit conversions are NOT DCP: ddiv's
# sequence is ~35 instructions with five scratch registers, and neither the
# DCP nor FPv5-SP has a 64-bit integer port at all.  Those come from the
# bit-exact soft-float sources, compiled into THIS archive rather than left
# for the linker to find: an FP runtime has to define the whole __aeabi_ set
# by itself.  A single missing symbol (this was __aeabi_ddiv) makes the linker
# reach for libgcc's ieee754-df.o, which also defines dadd/dsub/dmul and
# collides with the DCP versions above.  `make check-self-contained` guards it.
#
# Only objects with no overlap are borrowed.  conv.c and dconv.c are excluded
# because their 32-bit conversions are done on hardware here -- the 64-bit
# ones were split into conv64.c precisely so they could be picked up alone.

include ../../../../config.mak

FP_CC ?= $(CC)
FP_CFLAGS ?= $(CFLAGS)

SRCS = dcp_init.c
ASMS = dcp_aeabi.S

# Borrowed verbatim from the soft-float runtime; see above.
SOFT_DIR   = ../../soft
SOFT_SRCS  = ddiv.c fcmp.c conv64.c
SOFT_ASMS  = fcmp_asm.S

BUILD_DIR ?= build
OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o)) $(addprefix $(BUILD_DIR)/,$(ASMS:.S=.o)) \
       $(addprefix $(BUILD_DIR)/,$(SOFT_SRCS:.c=.o)) $(addprefix $(BUILD_DIR)/,$(SOFT_ASMS:.S=.o))

# -mfpu=rp2350 is what enables both the CP4 coprocessor encodings and the
# FPv5-SP vfp_sp feature bit in the assembler.
override FP_CFLAGS += -O2 -Wall -Wextra -mcpu=cortex-m33 -mthumb -mfloat-abi=soft -mfpu=rp2350 \
                      -DRP2350_DCP_ENABLED -I../../.. -I../../../../include

all: $(BUILD_DIR) $(OBJS)

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: %.c
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SOFT_DIR)/%.c
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SOFT_DIR)/%.S
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
