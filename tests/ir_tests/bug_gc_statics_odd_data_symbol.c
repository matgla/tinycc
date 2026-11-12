/*
 *  TCC Tests - End-of-TU static GC: data symbols at odd .rodata offsets
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* gc_unreferenced_statics masked the Thumb bit off every symbol value, data
 * included, so a dead object at an odd .rodata offset was erased from one byte
 * early, taking the last byte of the live object before it (Zig's build runner
 * got "c\0<digest>" for "c/<digest>").  Only code symbols carry the bit now. */
#include <stdio.h>

struct arr_1 { char a[1]; };
struct arr_2 { char a[2]; };
struct arr_5 { char a[5]; };
struct arr_23 { char a[24]; };

static const struct arr_1 one = {"v"};
static const struct arr_2 cs = {"c/"};
static const struct arr_23 dead = {"Reusing Cache Artifacts"};
static const struct arr_5 after = {"tail"};
static const struct arr_2 cs2 = {"x/"};
static const struct arr_5 dead2 = {"gone"};
static const struct arr_2 cs3 = {"y/"};

struct slice { const char *ptr; unsigned len; };

/* Inlined; the name is never read, so the optimizer leaves `dead` unreferenced
 * and the end-of-TU GC erases it. */
static inline unsigned start_node(unsigned node, struct slice name)
{
  (void)name;
  return node == 255 ? 255 : node;
}

__attribute__((noinline)) static void show(const char *p, unsigned n)
{
  for (unsigned i = 0; i < n; i++)
    printf("%02x", (unsigned char)p[i]);
  printf(" %.*s\n", (int)n, p);
}

int main(void)
{
  volatile unsigned node = 255;
  unsigned r = start_node(node, (struct slice){dead.a, 23});
  r += start_node(node, (struct slice){dead2.a, 4});
  show(one.a, 1);
  show(cs.a, 2);
  show(after.a, 4);
  show(cs2.a, 2);
  show(cs3.a, 2);
  printf("node %u\n", r);
  return 0;
}
