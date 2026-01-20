#!/bin/sh

TARGET=arm-none-eabi

mkdir -p newlib_build
cd newlib_build
export CFLAGS_FOR_TARGET='-g -Os -mfloat-abi=hard -mfpu=fpv5-sp-d16 -ffunction-sections -fdata-sections -mcpu=cortex-m33'
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

make -j8