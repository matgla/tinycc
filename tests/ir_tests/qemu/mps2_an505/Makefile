CC := arm-none-eabi-gcc
#../../../../armv8m-tcc

CFLAGS = -g -fvisibility=hidden -nostdlib -mcpu=cortex-m33
LDFLAGS = -g -fvisibility=hidden -nostdlib \
		  -L/usr/arm-none-eabi/lib/thumb/v8-m.main+dp/softfp $(LIBS) -Wl,-Ttext=0x10000000

ifeq ($(CC),../../../../armv8m-tcc)
LIBS = -llibc.a -llibrdimon.a -Wl,-oformat=elf32-littlearm
else
LIBS = -lc -specs=rdimon.specs -Tlinker_script.ld
endif

SRCS = $(wildcard *.c)
OBJS = $(patsubst %.c, build/%.o, $(SRCS))

TARGET = build/test


# Rules
all: $(TARGET)

build/%.o: %.c
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC)  $^ /usr/arm-none-eabi/lib/thumb/v8-m.main+dp/softfp/rdimon-crt0.o -o $@ $(LDFLAGS)

clean:
	rm -rf build