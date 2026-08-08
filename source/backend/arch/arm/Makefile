# ARM Architecture build
#
# Called from arch/Makefile:
#   make -C arm TOP=... BUILD_DIR=... CC=... CFLAGS=... DEFINES=...
#
# Builds architecture-level objects (arm.c, arm_aapcs.c),
# dispatches to ISA subdirectories (thumb/), and bundles
# everything into $(BUILD_DIR)/libarm.a.

TOP       ?= ../../..
BUILD_DIR ?= .
CC        ?= gcc
AR        ?= ar
CFLAGS    ?=
DEFINES   ?=

SRCS = arm.c arm_aapcs.c arm_regalloc.c ssa_opt_arm.c arm-link.c
OBJS = $(addprefix $(BUILD_DIR)/, $(SRCS:.c=.o))

# Headers these TUs see.  Without them a `make` after any tcc.h / config.h edit
# reports nothing to do and relinks objects built against the previous struct
# layout — the top-level Makefile drops the whole object dir when config.h or
# config.mak moves, but that guard cannot see an ordinary header edit.
CORE_HDRS = $(wildcard $(TOP)/*.h) \
	$(wildcard $(TOP)/source/include/*.h) \
	$(wildcard $(TOP)/source/driver/*.h) \
	$(wildcard $(TOP)/source/frontend/*.h) \
	$(wildcard $(TOP)/source/ir/*.h) \
	$(wildcard $(TOP)/source/machine/*.h) \
	$(wildcard $(TOP)/source/obj/*.h) \
	$(wildcard $(TOP)/source/support/*.h) \
	$(wildcard $(TOP)/source/opt/include/*.h) \
	$(wildcard $(TOP)/source/opt/framework/*.h) \
	$(wildcard $(TOP)/source/memory/include/memory/*.h) \
	$(wildcard $(TOP)/source/backend/arch/arm/*.h) \
	$(wildcard $(TOP)/source/backend/arch/arm/thumb/*.h)

# Per-FPU capability tables (arch/fpu/arm/).  These were previously not built
# at all: their headers declared `const FloatingPointConfig x;` with no extern,
# which is a tentative definition, so every includer quietly got its own
# zero-initialised copy and the real tables were dead code.  The headers now
# say `extern`, so the definitions have to come from here.
FPU_SRCS = fpv5-sp-d16.c fpv5-d16.c rp2350-dcp.c
FPU_OBJS = $(addprefix $(BUILD_DIR)/fpu-, $(FPU_SRCS:.c=.o))
OBJS += $(FPU_OBJS)

$(BUILD_DIR)/fpu-%.o: $(TOP)/source/backend/arch/fpu/arm/%.c $(CORE_HDRS)
	@mkdir -p $(dir $@)
	$(CC) -o $@ -c $< $(CFLAGS) $(DEFINES) -I$(TOP) -I$(TOP)/source/ir 

ISA_DIRS = thumb
ISA_LIBS = $(foreach d,$(ISA_DIRS),$(BUILD_DIR)/lib$(d).a)

LIB = $(BUILD_DIR)/libarm.a

all: $(LIB)

$(LIB): $(OBJS) $(ISA_LIBS)
	printf 'create $@\n' > $(BUILD_DIR)/_libarm.mri
	@for o in $(OBJS); do printf 'addmod %s\n' $$o >> $(BUILD_DIR)/_libarm.mri; done
	@for l in $(ISA_LIBS); do printf 'addlib %s\n' $$l >> $(BUILD_DIR)/_libarm.mri; done
	@printf 'save\nend\n' >> $(BUILD_DIR)/_libarm.mri
	$(AR) -M < $(BUILD_DIR)/_libarm.mri
	@rm -f $(BUILD_DIR)/_libarm.mri

$(BUILD_DIR)/lib%.a: FORCE
	$(MAKE) --no-print-directory -C $* \
		TOP=$(TOP) BUILD_DIR=$(BUILD_DIR)

$(BUILD_DIR)/%.o: %.c $(CORE_HDRS)
	@mkdir -p $(dir $@)
	$(CC) -o $@ -c $< $(CFLAGS) $(DEFINES) -I$(TOP) -I$(TOP)/source/ir 

clean:
	rm -f $(OBJS) $(LIB) $(ISA_LIBS)
	@for dir in $(ISA_DIRS); do \
		$(MAKE) --no-print-directory -C $$dir clean BUILD_DIR=$(BUILD_DIR); \
	done

FORCE:
.PHONY: all clean FORCE
