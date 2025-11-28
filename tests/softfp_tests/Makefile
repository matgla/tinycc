CC = gcc
CFLAGS = -Wall -pedantic -g -std=c11

SRCS = $(wildcard *.c) ../../lib/armv8m_soft_fp.c
OBJS = $(patsubst %.c, build/%.o, $(notdir $(SRCS)))


TARGET = build/softfp_tests

# Tell Make where to find source files
vpath %.c . ../../lib

# Rules
all: $(TARGET) test

build/%.o: %.c
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

test: $(TARGET)
	./$(TARGET)

build_tests: $(TARGET)

clean:
	rm -rf build
	$(MAKE) -C .. clean
