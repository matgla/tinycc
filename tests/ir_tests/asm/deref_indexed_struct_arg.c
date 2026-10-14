/*
 *  TCC asm fixture - scaled-index derefs: struct arguments vs scalar operands
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
struct S4 { int a; };
struct S8 { const int *p; int n; };

int f4(struct S4 s);
int f8(struct S8 s);

struct S4 a4[8];
struct S8 a8[8];
int ints[8];

/* The struct's address reaches the call: no load of the element's first
 * word followed by a load through it. */
int pass4(int i) { return f4(a4[i]); }
int pass8(int i) { return f8(a8[i]); }

/* A scalar element read as an ALU operand still folds into [Rn, Rm, lsl #2]. */
int add_elem(int i, int v) { return (ints[i] ^ v) + 1; }
