/*
 *  TCC Tests - ipa:pure_via_sret's flag does not hide a nested function
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* ipa:pure_via_sret set its flag in the function identifier Sym's FuncAttr,
 * which shares its word with sym_scope: the file-scope mk read as block
 * scope, a nested mk was linked below it and user() called the outer mk.
 * The flag is now SymAttr.pure_via_sret. */
#include <stdio.h>

#define NI __attribute__((noinline))

struct P { int a, b, c; };
static NI struct P mk(int x) { struct P p = {x, x, x}; return p; }
static NI struct P mk2(int x) { struct P p = {x * 2, x, 0}; return p; }
static NI int use_mk(int y) { return mk(y).a; }

/* a dead call to the flagged mk (before any nested mk) */
static int calls;
static NI int counted(void) { return ++calls; }
static NI int drop_dead(int y)
{
  mk(y);
  return counted();
}

static NI int user(int y)
{
  struct P mk(int z) { struct P p = {z + 100, 0, 0}; return p; }
  return mk(y).a;
}

/* a nested function in a block, while the outer one is flagged */
static NI int user2(int y)
{
  int r = mk2(y).a;
  {
    struct P mk2(int z) { struct P p = {z + 500, 0, 0}; return p; }
    r += mk2(y).a;
  }
  return r;
}

int main(void)
{
  volatile int one = 1;
  printf("%d %d %d\n", use_mk(one), user(one), user2(one));
  printf("%d\n", drop_dead(one));
  return 0;
}
