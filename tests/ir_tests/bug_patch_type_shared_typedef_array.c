/*
 * Completing `extern T a;` (T an unsized-array typedef) with `int a[10];`
 * must not size the typedef itself (patch_type in attr_merge.c wrote through
 * the shared array Sym; the symbol now gets its own).
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

typedef int T[];
extern T a;
int a[10];
T c = {1, 2, 3};

typedef char U[];
extern U u;
char u[7];
U v = "ab";
U w = {1, 2, 3, 4, 5};

typedef int V[];
V d = {1, 2};
extern V e;
int e[5];
V f = {1, 2, 3, 4};

int main(void)
{
  printf("sizeof a=%d\n", (int)sizeof a);
  printf("sizeof c=%d\n", (int)sizeof c);
  printf("sizeof u=%d\n", (int)sizeof u);
  printf("sizeof v=%d\n", (int)sizeof v);
  printf("sizeof w=%d\n", (int)sizeof w);
  printf("sizeof d=%d\n", (int)sizeof d);
  printf("sizeof e=%d\n", (int)sizeof e);
  printf("sizeof f=%d\n", (int)sizeof f);
  return 0;
}
