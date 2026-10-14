/*
 *  TCC IR - Spill reload after an outlined window
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

// Keep all three bodies: the third occurrence makes outlining profitable at -Os.
#include <stdio.h>
typedef struct Node { struct Node *next; int v; int k[6]; } Node;
__attribute__((noinline)) int g4(int a, int b, int c, int d) { return a * 3 + b * 5 + c * 7 + d * 11; }
__attribute__((noinline)) int g6(int a, int b, int c, int d, int e, int f) { return a - b + c - d + e - f; }
#define H(name, CALL) \
__attribute__((noinline)) int name(Node *n, int **pp, int i, int j, int x) \
{ \
  int v0 = n->k[0] * 3 + x; \
  int v1 = n->k[1] * 4 + x; \
  int v2 = n->k[2] * 5 + x; \
  int v3 = n->k[3] * 6 + x; \
  int v4 = n->k[4] * 7 + x; \
  int v5 = n->k[5] * 8 + x; \
  int r = CALL; \
  return r + (v0 + v1 * 2 + v2 * 3 + v3 * 4 + v4 * 5 + v5 * 6) + n->next->v + *pp[i]; \
}
H(f1, g4(x, *pp[i], *pp[j], i))
H(f2, g4(x, *pp[i], *pp[j], i))
H(f3, g6(*pp[i], *pp[j], n->v, n->next->v, *pp[i] + 1, x))
int main(void)
{
  static Node nodes[2];
  for (int i = 0; i < 2; i++) {
    nodes[i].next = &nodes[1 - i];
    nodes[i].v = i + 1;
    for (int t = 0; t < 6; t++) nodes[i].k[t] = t + 1;
  }
  static int vals[2] = {10, 20};
  int *pp[2] = {&vals[0], &vals[1]};
  printf("%d %d %d\n", f1(&nodes[0], pp, 0, 1, 1), f2(&nodes[0], pp, 0, 1, 1), f3(&nodes[0], pp, 0, 1, 1));
  return 0;
}
