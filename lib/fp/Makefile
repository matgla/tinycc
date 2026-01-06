# Floating Point Library Makefile
# Builds multiple FP library variants for different ARM FPU configurations
#
# Usage:
#   make FPU=soft          - Pure software floating point (Cortex-M0/M3)
#   make FPU=vfpv4-sp      - VFPv4 single-precision (Cortex-M4F)
#   make FPU=vfpv5-dp      - VFPv5 double-precision (Cortex-M7)
#   make FPU=rp2350        - RP2350 with double coprocessor

include ../../config.mak

# Default to soft float if not specified
FPU ?= soft
TARGET ?= arm

# Library output names with target architecture
SOFT_LIB = libtcc1-fp-soft-$(TARGET).a
VFPV4SP_LIB = libtcc1-fp-vfpv4-sp-$(TARGET).a
VFPV5DP_LIB = libtcc1-fp-vfpv5-dp-$(TARGET).a
RP2350_LIB = libtcc1-fp-rp2350-$(TARGET).a

# Determine which library and sources to build
ifeq ($(FPU),soft)
  TARGET_LIB = $(SOFT_LIB)
  TARGET_DIR = soft
else ifeq ($(FPU),vfpv4-sp)
  TARGET_LIB = $(VFPV4SP_LIB)
  TARGET_DIR = arm/vfpv4-sp
else ifeq ($(FPU),vfpv5-dp)
  TARGET_LIB = $(VFPV5DP_LIB)
  TARGET_DIR = arm/vfpv5-dp
else ifeq ($(FPU),rp2350)
  TARGET_LIB = $(RP2350_LIB)
  TARGET_DIR = arm/rp2350
else
  $(error Invalid FPU option: $(FPU). Use: soft, vfpv4-sp, vfpv5-dp, or rp2350)
endif

BUILD_DIR = build

all: build

# Build exactly one variant selected by FPU and TARGET_* vars
build:
	@echo "Building FP library for $(FPU)..."
	@mkdir -p $(BUILD_DIR)/$(FPU)
	$(MAKE) -C $(TARGET_DIR) BUILD_DIR=$(CURDIR)/$(BUILD_DIR)/$(FPU) \
		CFLAGS="$(CFLAGS) -I$(CURDIR)/../../include"
	$(AR) rcs $(TARGET_LIB) $(BUILD_DIR)/$(FPU)/*.o
	@echo "Created $(TARGET_LIB)"

# Build all variants
all-variants:
	@$(MAKE) FPU=soft build
	@$(MAKE) FPU=vfpv4-sp build
	@$(MAKE) FPU=vfpv5-dp build
	@$(MAKE) FPU=rp2350 build

clean:
	$(MAKE) -C soft clean
	$(MAKE) -C arm/vfpv4-sp clean
	$(MAKE) -C arm/vfpv5-dp clean
	$(MAKE) -C arm/rp2350 clean
	rm -f $(SOFT_LIB) $(VFPV4SP_LIB) $(VFPV5DP_LIB) $(RP2350_LIB)

.PHONY: all build all-variants clean
