#!/bin/sh

arm-none-eabi-gcc -mcpu=cortex-m33 $1.S -o expected/$1_gcc.o -nostartfiles -Wl,--entry=0 -Wl,--section-start=.text=0
arm-none-eabi-objcopy --only-section=.text expected/$1_gcc.o expected/$1.o
rm expected/$1_gcc.o