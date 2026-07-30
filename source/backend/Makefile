# Backend build dispatcher
#
# Called from top-level Makefile:
#   make -C source/backend ARCH=arm BUILD_DIR=... CC=... CFLAGS=... DEFINES=...
#
# To add a new architecture:
#   1. Create source/backend/arch/<name>/ with sources and a Makefile
#   2. Add <name>_FILES and ARCH_OBJS_<name> to the top-level Makefile

ARCH     ?= arm
TOP      ?= ..
BUILD_DIR ?= .

all:
	$(MAKE) -C arch/$(ARCH) TOP=$(TOP) BUILD_DIR=$(BUILD_DIR)

clean:
	$(MAKE) -C arch/$(ARCH) clean BUILD_DIR=$(BUILD_DIR)

.PHONY: all clean
