#!/bin/sh
#
# Build newlib for one float ABI.
#
#   sh build_newlib.sh [soft|softfp|hard] [fpu]
#
# Each build is a single variant (--disable-multilib), so there is one tree per
# ABI: newlib_build for soft (the historical name) and newlib_build-<abi> for
# the others.  The ABI must match the code linked against it — a hard-float
# caller passes float arguments in s0-s15, which a soft-float libm would read
# from GPRs.  Normally only the soft tree is needed: for softfp/hard the
# toolchain already ships a matching multilib (see USE_NEWLIB_BUILD in the
# Makefile), so this is here for targets the toolchain does not cover.

set -e

TARGET=arm-none-eabi
FLOAT_ABI=${1:-soft}
FPU=${2:-fpv5-sp-d16}

case "$FLOAT_ABI" in
  soft)
    BUILD_DIR=newlib_build
    FLOAT_FLAGS="-mfloat-abi=soft"
    ;;
  softfp | hard)
    BUILD_DIR=newlib_build-$FLOAT_ABI
    FLOAT_FLAGS="-mfloat-abi=$FLOAT_ABI -mfpu=$FPU"
    ;;
  *)
    echo "build_newlib.sh: unknown float ABI '$FLOAT_ABI' (want soft, softfp or hard)" >&2
    exit 1
    ;;
esac

echo "Building newlib for -mfloat-abi=$FLOAT_ABI into $BUILD_DIR"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
export CFLAGS_FOR_TARGET="-g -Os $FLOAT_FLAGS -ffunction-sections -fdata-sections -mcpu=cortex-m33"
../libs/newlib/configure \
    --target=$TARGET \
    --prefix=$PWD/newlib_install \
    --disable-newlib-supplied-syscalls \
    --enable-newlib-reent-small \
    --enable-newlib-retargetable-locking \
    --disable-newlib-fvwrite-in-streamio \
    --disable-newlib-fseek-optimization \
    --disable-newlib-wide-orient \
    --enable-newlib-nano-malloc \
    --disable-newlib-unbuf-stream-opt \
    --enable-lite-exit \
    --enable-newlib-global-atexit \
    --disable-newlib-nano-formatted-io \
    --disable-multilib \
    --disable-nls \
    --enable-newlib-io-long-long \
    --enable-newlib-io-long-double \
    --enable-newlib-io-float \
    --enable-newlib-io-c99-formats \

make -j8
