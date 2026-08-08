# ARM VFPv4/FPv5 single-precision floating-point runtime.
#
# Single precision runs on the hardware FPU (Cortex-M4F's FPv4-SP-D16 and the
# Cortex-M33's FPv5-SP-D16 are the same unit as far as these ops care).  There
# is no double unit, so every `double` entry point comes from the bit-exact
# soft-float sources -- compiled into THIS archive rather than left for the
# linker to find.
#
# An FP runtime has to define the whole __aeabi_ set by itself: a single missing
# symbol makes the linker reach for libgcc's ieee754-{sf,df}.o, whose objects
# bundle several entry points each, so it drags in duplicates of ones defined
# here and the link dies on "multiple definition".  `make check-self-contained`
# guards it.  (This archive used to be 20 symbols short and only linked at all
# because something else on the command line happened to supply the gap.)
#
# Only objects with no symbol overlap are borrowed: soft/conv.c is excluded
# because its 32-bit conversions are done in hardware here, and soft/fadd.c,
# fmul.c and fdiv.c are excluded for the same reason.  The compares are NOT
# hardware here -- see below.

include ../../../../config.mak

FP_CC ?= $(CC)
FP_CFLAGS ?= $(CFLAGS)

# Single-precision hardware operations (inline VFP asm).
#
# fcmp.c is deliberately absent: the local hardware version returned FPSCR in
# r0 and never set the APSR flags, while __aeabi_cfcmp* is defined by its
# effect on the flags.  The soft compares (fcmp.c + fcmp_asm.S below) are in
# the flag convention the backend actually tests -- flags as if `cmp r,#0` on a
# -1/0/1/2 result -- and the compiler never inlines a compare on this unit
# (`has_fcmp = 0` in fpv5-sp-d16.c), so nothing is lost by taking the soft one.
SRCS = fops.c conv.c

# Borrowed verbatim from the soft-float runtime; see above.
SOFT_DIR  = ../../soft
SOFT_SRCS = dadd.c dmul.c ddiv.c dcmp.c dconv.c fcmp.c conv64.c
SOFT_ASMS = dcmp_asm.S fcmp_asm.S f2d_stub.S

BUILD_DIR ?= build
OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o)) \
       $(addprefix $(BUILD_DIR)/,$(SOFT_SRCS:.c=.o)) \
       $(addprefix $(BUILD_DIR)/,$(SOFT_ASMS:.S=.o))

# -mfloat-abi=soft is load-bearing, and for the same reason rp2350 passes it:
# this is the FP runtime itself.  Without it the ABI defaults to softfp, which
# together with the -mfpu below tells the compiler it may lower `a + b` inline
# -- inside the very functions that implement `a + b`.  `soft` means "emit no
# FPU instructions", which is exactly right for C sources that must stay
# bit-exact software.  The hardware ops here are unaffected: they are inline asm
# with "r" constraints, and FPU instructions in asm are gated by -mfpu, not by
# the ABI.  Argument passing is identical under soft and softfp, so callers
# built either way link against this archive unchanged.
override FP_CFLAGS += -O2 -Wall -Wextra -mcpu=cortex-m33 -mthumb -mfloat-abi=soft \
                      -mfpu=fpv5-sp-d16 -I../../.. -I../../../../include

all: $(BUILD_DIR) $(OBJS)

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: %.c
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SOFT_DIR)/%.c
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SOFT_DIR)/%.S
	$(FP_CC) $(FP_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
