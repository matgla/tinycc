# Architecture build dispatcher
#
# Called from top-level Makefile:
#   make -C arch ARCH=arm BUILD_DIR=... CC=... CFLAGS=... DEFINES=...
#
# To add a new architecture:
#   1. Create arch/<name>/ with sources and a Makefile
#   2. Add <name>_FILES and ARCH_OBJS_<name> to the top-level Makefile

ARCH     ?= arm
TOP      ?= ..
BUILD_DIR ?= .

all:
	$(MAKE) -C $(ARCH) TOP=$(TOP) BUILD_DIR=$(BUILD_DIR) \
		CC="$(CC)" AR="$(AR)" CFLAGS="$(CFLAGS)" DEFINES="$(DEFINES)"

clean:
	$(MAKE) -C $(ARCH) clean BUILD_DIR=$(BUILD_DIR)

.PHONY: all clean
