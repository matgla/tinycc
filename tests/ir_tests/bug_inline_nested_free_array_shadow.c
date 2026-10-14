/*
 * Regression test: a nested token-replay inline bound the callee's array W to
 * a scalar local W of the enclosing inlined body ("pointer expected").  Fix:
 * a callee's free name resolving to a caller local keeps the call.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

static int W[4] = {1, 2, 3, 4};
static int sg(int x) { return W[x & 3]; }
int sg2(int x) { int W = 9; return sg(x) + W; }
static int sg3(int x) { int W[2] = {50, 60}; return sg(x) + W[x & 1]; }

int direct(int x)
{
  int W = 100;
  return sg(x) + W;
}

int main(void)
{
  volatile int one = 1;
  printf("%d %d %d\n", sg2(one + 1), sg3(one), direct(one + 2));
  return 0;
}
