/*
 * sizeof of a file-scope compound literal must not allocate the literal's storage.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdint.h>

int x = sizeof((int[]){1, 2, 3});
int y = sizeof((int[]){1});
int z = 5;

int main(void)
{
  /* Nothing may sit between the three ints: the literals are never emitted. */
  long d1 = (char *)&y - (char *)&x;
  long d2 = (char *)&z - (char *)&y;
  printf("%d %d\n", x, y);
  printf("%s\n", (d1 == 4 || d1 == -4) && (d2 == 4 || d2 == -4) ? "packed" : "padded");
  return 0;
}
