# Thumb instruction set build
#
# Called from arch/arm/Makefile:
#   make -C thumb TOP=... BUILD_DIR=... CC=... CFLAGS=... DEFINES=...
#
# Produces $(BUILD_DIR)/libthumb.a

TOP       ?= ../../..
BUILD_DIR ?= .
CC        ?= gcc
AR        ?= ar
CFLAGS    ?=
DEFINES   ?=

SRCS = thumb.c thop_alu_imm.c thop_alu_reg.c thop_cmp.c thop_shift_imm.c thop_shift_reg.c thop_mem_reg.c thop_mem_unpriv.c thop_mem_exclusive.c thop_mem_imm.c thop_extend.c thop_rev.c thop_bitfield.c thop_vfp.c thop_system.c thop_branch.c thop_block.c thop_mul.c thop_dsp.c thop_tbb.c thop_ldrd.c thop_ldrex.c thop_ldaex.c thop_mrs.c thop_pld.c thop_mov.c thop_mvn.c thop_adr.c thop_ldr_literal.c
OBJS = $(addprefix $(BUILD_DIR)/, $(SRCS:.c=.o))
LIB  = $(BUILD_DIR)/libthumb.a

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -o $@ -c $< $(CFLAGS) $(DEFINES) -I$(TOP) -I$(TOP)/ir

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean
