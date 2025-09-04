#!/bin/sh

current_dir=$(dirname "$0")
SCRIPT_DIR=$(realpath "$current_dir")

build_cross_compiler()
{
  echo "Building cross compiler..."
  mkdir -p bin
  ./configure --extra-cflags="-DTCC_DEBUG=0 -g -O0 -DTARGETOS_YasOS=1" --enable-cross --config-asm=yes --config-bcheck=no --config-pie=yes --config-pic=yes 
  if [ $? -ne 0 ]; then
    exit -1;
  fi
  make -j$(nproc)
  if [ $? -ne 0 ]; then
    exit -1;
  fi
  cp armv8m-tcc bin
}

cd ../../.. 
build_cross_compiler
if [ $? -ne 0 ]; then
  echo "Failed to build cross compiler"

  cd $SCRIPT_DIR
  exit 1
fi

cd $SCRIPT_DIR
export TEST_CC="$SCRIPT_DIR/../../../bin/armv8m-tcc"
export TEST_COMPARE_CC="arm-none-eabi-gcc"
export TEST_OBJDUMP="arm-none-eabi-objdump"
export TEST_OBJCOPY="arm-none-eabi-objcopy"
pytest
if [ $? -ne 0 ]; then
  echo "Tests failed"
  exit 1
fi

