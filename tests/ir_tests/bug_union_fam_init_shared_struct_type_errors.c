/* A struct member's flexible array initialised through a union must fit in
 * the union: more elements than it has room for is a diagnostic, not an
 * "initializer overflow" internal error (init/initializer.c decl_design_flex).
 *
 * Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */
struct F { int n; int a[]; };
union U { struct F s; int x[4]; };
struct A { int a; char b[]; };
union V { struct A a; char buf[8]; };

#if defined FAM_UNION_OVERFLOW
union U u = { .s = { 1, { 2, 3, 4, 5 } } };
#elif defined FAM_UNION_DESIGNATED_OVERFLOW
union U u = { .s = { 1, { [3] = 5 } } };
#elif defined FAM_UNION_STRING_OVERFLOW
union V v = { .a = { 1, "abcd" } };
#elif defined FAM_UNION_LOCAL_OVERFLOW
int f(void) { union U l = { .s = { 1, { 2, 3, 4, 5, 6 } } }; return l.x[1]; }
#elif defined FAM_UNION_FITS
union U u = { .s = { 1, { 2, 3, 4 } } };
union V v = { .a = { 1, "abc" } };
#endif

int main(void)
{
#if defined FAM_UNION_FITS
  return u.x[3] + v.buf[6] - 'c' - 4;
#else
  return 0;
#endif
}
