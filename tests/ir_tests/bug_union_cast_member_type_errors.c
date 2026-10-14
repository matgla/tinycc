/* GNU cast to union from a type that no member has: gcc rejects it with
 * "cast to union type from type not present in union"; tcc silently converted
 * to the first member (expr/unary.c unary_paren).
 *
 * Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */
#include <stdio.h>

struct S { short a, b; };
struct T { short a, b; };
union U { char c; int i; double d; struct S s; int *p; };

#if defined CAST_SHORT
int f(short x) { union U u = (union U)x; return u.i; }
#elif defined CAST_LONG
int f(long x) { union U u = (union U)x; return u.i; }
#elif defined CAST_UNSIGNED
int f(unsigned x) { union U u = (union U)x; return u.i; }
#elif defined CAST_OTHER_STRUCT
int f(struct T x) { union U u = (union U)x; return u.i; }
#elif defined CAST_FUNC
int g(void);
int f(void) { union U u = (union U)g; return u.i; }
#elif defined CAST_STATIC
union U gu = (union U)1.5f;
#elif defined CAST_OK
int f(int x) { union U u = (union U)x; return u.i; }
#endif

int main(void)
{
#if defined CAST_OK
  printf("ok %d\n", f(5));
#endif
  return 0;
}
