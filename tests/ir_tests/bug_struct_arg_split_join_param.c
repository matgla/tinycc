/* ra:struct_arg_split forwarded a by-value struct argument from the stores
 * of only one arm when its PARAM was the join of two.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Under hard float SRA leaves a by-value struct argument in its frame slot and
 * ra:struct_arg_split turns the slot into word PARAMs.  Here both arms of an
 * `if` write the slot and the PARAM reading it is the join itself (Zig's
 * `const s = it.next() orelse "0"; parseInt(s, 10)`, fs.fstab's dump/pass
 * fields): the backward scan for the feeding stores started above the jump
 * target and found the else arm's `{"7", 1}`, so the then arm passed its
 * pointer with length 1 (`123` parsed as 1).  In the kernel the pointer word
 * was lost too and fstab parsing faulted at boot. */
#include <stddef.h>
#include <stdio.h>

struct slice
{
  const char *ptr;
  unsigned len;
};

static const char *const toks[] = {"123", "45", NULL};

__attribute__((noinline)) struct slice next_tok(int *i)
{
  const char *t = toks[*i];
  if (!t)
    return (struct slice){NULL, 0};
  ++*i;
  unsigned n = 0;
  while (t[n])
    n++;
  return (struct slice){t, n};
}

__attribute__((noinline)) int parse(struct slice s, int base)
{
  int v = 0;
  for (unsigned k = 0; k < s.len; k++)
    v = v * base + (s.ptr[k] - '0');
  return v;
}

__attribute__((noinline)) int field(int *i)
{
  struct slice t19, t22;
  t19 = next_tok(i);
  if (t19.ptr != NULL)
  {
    t22 = t19;
    goto join;
  }
  t22 = (struct slice){"7", 1};
  goto join;
join:
  return parse(t22, 10);
}

/* The same with a second argument word after the join and a reused slot. */
__attribute__((noinline)) int two_fields(int *i)
{
  struct slice t19, t22, t24;
  int a, b;
  t19 = next_tok(i);
  if (t19.ptr != NULL)
  {
    t24 = t19;
    t22 = t24;
    goto j1;
  }
  t22 = (struct slice){"9", 1};
j1:
  a = parse(t22, 10);
  t19 = next_tok(i);
  if (t19.ptr != NULL)
  {
    t22 = t19;
    t24 = t22;
    goto j2;
  }
  t24 = (struct slice){"8", 1};
j2:
  b = parse(t24, 10);
  return a * 1000 + b;
}

int main(void)
{
  int i = 0;
  int a = field(&i);
  int b = field(&i);
  int c = field(&i);
  printf("field %d %d %d\n", a, b, c);
  i = 0;
  int d = two_fields(&i);
  int e = two_fields(&i);
  printf("two_fields %d %d\n", d, e);
  return 0;
}
