CC := ../../../../armv8m-tcc

ARM_SYSROOT = $(shell arm-none-eabi-gcc --print-sysroot)
SYSROOT = $(shell realpath $(ARM_SYSROOT)/lib/thumb/v8-m.base/nofp)
CRT_GCC = $(ARM_SYSROOT)/../lib/gcc/arm-none-eabi/14.2.1/thumb/v8-m.base/nofp

CFLAGS = -nostdlib -g -fvisibility=hidden -mcpu=cortex-m33
LDFLAGS = $(CFLAGS) -g -fvisibility=hidden 

ifeq ($(CC),../../../../armv8m-tcc)
CFLAGS += -I$(ARM_SYSROOT)/include -I../../../../include
LDFLAGS += -L$(SYSROOT) -llibc.a -llibrdimon.a -Wl,-oformat=elf32-littlearm -Wl,-Ttext=0x10000000
else
LDFLAGS += -Wl,--start-group -lrdimon -lc_nano -lgcc -Wl,--end-group --specs=rdimon.specs -Tlinker_script.ld
endif

SRCS = $(wildcard *.c)
ASMS = $(wildcard *.s)
OBJS = $(patsubst %.c, build/%.o, $(SRCS)) 
OBJS += $(patsubst %.s, build/%.o, $(ASMS))

CRT_LIBS =  $(CRT_GCC)/crti.o $(CRT_GCC)/crtbegin.o $(SYSROOT)/rdimon-crt0.o $(CRT_GCC)/crtend.o $(CRT_GCC)/crtn.o

TARGET = build/test

# Rules
all: $(TARGET)

build/%.o: %.c
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.s
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $^ $(CRT_LIBS) -o $@ $(LDFLAGS)

clean:
	rm -rf build