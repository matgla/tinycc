# Floating Point Library Makefile
# Builds multiple FP library variants for different ARM FPU configurations
#
# Usage:
#   make FPU=soft          - Pure software floating point (Cortex-M0/M3)
#   make FPU=vfpv4-sp      - VFPv4 single-precision (Cortex-M4F)
#   make FPU=vfpv5-dp      - VFPv5 double-precision (Cortex-M7)
#   make FPU=rp2350        - RP2350 with double coprocessor

include ../../config.mak

# Cross-build knobs (so we don't accidentally inherit host-only flags like ASAN)
FP_CC ?= $(CC)
FP_AR ?= $(AR)
FP_CFLAGS ?= $(CFLAGS)

# Default to soft float if not specified
FPU ?= soft
TARGET ?= arm

# Short canonical library names (used for both static and shared)
# These are the names used by -l flags: -lsoftfp, -lvfpv4sp, etc.
SOFT_NAME    = softfp
VFPV4SP_NAME = vfpv4sp
VFPV5DP_NAME = vfpv5dp
RP2350_NAME  = rp2350fp

# Backward-compatible long names (created as symlinks)
SOFT_COMPAT    = libtcc1-fp-soft-$(TARGET).a
VFPV4SP_COMPAT = libtcc1-fp-vfpv4-sp-$(TARGET).a
VFPV5DP_COMPAT = libtcc1-fp-vfpv5-dp-$(TARGET).a
RP2350_COMPAT  = libtcc1-fp-rp2350-$(TARGET).a

# Determine which library and sources to build
ifeq ($(FPU),soft)
  TARGET_NAME   = $(SOFT_NAME)
  TARGET_COMPAT = $(SOFT_COMPAT)
  TARGET_DIR    = soft
else ifeq ($(FPU),vfpv4-sp)
  TARGET_NAME   = $(VFPV4SP_NAME)
  TARGET_COMPAT = $(VFPV4SP_COMPAT)
  TARGET_DIR    = arm/vfpv4-sp
else ifeq ($(FPU),vfpv5-dp)
  TARGET_NAME   = $(VFPV5DP_NAME)
  TARGET_COMPAT = $(VFPV5DP_COMPAT)
  TARGET_DIR    = arm/vfpv5-dp
else ifeq ($(FPU),rp2350)
  TARGET_NAME   = $(RP2350_NAME)
  TARGET_COMPAT = $(RP2350_COMPAT)
  TARGET_DIR    = arm/rp2350
else
  $(error Invalid FPU option: $(FPU). Use: soft, vfpv4-sp, vfpv5-dp, or rp2350)
endif

TARGET_LIB = lib$(TARGET_NAME).a
TARGET_SO  = lib$(TARGET_NAME).so

BUILD_DIR = build

all: build

# Build exactly one variant selected by FPU and TARGET_* vars (static archive)
build:
	@echo "Building FP library for $(FPU)..."
	@mkdir -p $(BUILD_DIR)/$(FPU)
	$(MAKE) -C $(TARGET_DIR) BUILD_DIR=$(CURDIR)/$(BUILD_DIR)/$(FPU) \
		FP_CC="$(FP_CC)" FP_CFLAGS="$(FP_CFLAGS) -I$(CURDIR)/../../include"
	$(FP_AR) rcs $(TARGET_LIB) $(BUILD_DIR)/$(FPU)/*.o
	@# Backward-compatible symlink (e.g. libtcc1-fp-soft-armv8m.a -> libsoftfp.a)
	@ln -sf $(TARGET_LIB) $(TARGET_COMPAT)
	@echo "Created $(TARGET_LIB) (compat: $(TARGET_COMPAT))"

# Build shared library version of the current FPU variant
# Objects are rebuilt with -fPIC in a separate directory
# Uses -nodefaultlibs (not -nostdlib) since this IS the compiler runtime;
# we don't want tcc to try linking libtcc1/softfp into softfp itself.
# armeabi.c is compiled and linked in because yasld symbol resolution only
# searches self + children (not parent modules), so __aeabi_memset,
# __aeabi_lcmp, etc. must be statically included in the shared library.
build-shared:
	@echo "Building shared FP library for $(FPU)..."
	@mkdir -p $(BUILD_DIR)/$(FPU)-pic
	$(MAKE) -C $(TARGET_DIR) BUILD_DIR=$(CURDIR)/$(BUILD_DIR)/$(FPU)-pic \
		FP_CC="$(FP_CC)" FP_CFLAGS="$(FP_CFLAGS) -I$(CURDIR)/../../include -fPIC -fpic"
	$(FP_CC) $(FP_CFLAGS) -I$(CURDIR)/../../include -fPIC -fpic \
		-c $(CURDIR)/../armeabi.c -o $(BUILD_DIR)/$(FPU)-pic/armeabi.o
	$(FP_CC) -shared -fPIC -nodefaultlibs -Wl,-Ttext=0x0 -Wl,-section-alignment=0x4 \
		-Wl,-oformat=yaff \
		-o $(TARGET_SO) $(BUILD_DIR)/$(FPU)-pic/*.o
	@echo "Created $(TARGET_SO)"

# Build all variants (static)
all-variants:
	@$(MAKE) FPU=soft build
	@$(MAKE) FPU=vfpv4-sp build
	@$(MAKE) FPU=vfpv5-dp build
	@$(MAKE) FPU=rp2350 build

# Build all shared library variants
all-shared:
	@$(MAKE) FPU=soft build-shared
	@$(MAKE) FPU=vfpv4-sp build-shared
	@$(MAKE) FPU=vfpv5-dp build-shared
	@$(MAKE) FPU=rp2350 build-shared

clean:
	$(MAKE) -C soft clean
	$(MAKE) -C arm/vfpv4-sp clean
	$(MAKE) -C arm/vfpv5-dp clean
	$(MAKE) -C arm/rp2350 clean
	rm -rf $(BUILD_DIR)
	# Remove archives, shared libs, and compat symlinks
	rm -f libsoftfp.a libvfpv4sp.a libvfpv5dp.a librp2350fp.a
	rm -f libsoftfp.so libvfpv4sp.so libvfpv5dp.so librp2350fp.so
	rm -f libtcc1-fp-*.a

.PHONY: all build build-shared all-variants all-shared clean
