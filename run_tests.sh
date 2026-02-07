#!/bin/bash
export TEST_CC=/home/mateusz/repos/tinycc/armv8m-tcc
export TEST_COMPARE_CC=arm-none-eabi-gcc
export TEST_OBJDUMP=arm-none-eabi-objdump
export TEST_OBJCOPY=arm-none-eabi-objcopy
cd /home/mateusz/repos/tinycc/tests/thumb/armv8m
python3 -m pytest --tb=line -q .

