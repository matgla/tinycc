/*
 *  TCC Tests - internal functions return 8-byte aggregates in r0/r1
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

typedef unsigned u32;
typedef unsigned short u16;
struct eu { void *payload; u16 error; };     /* Zig's E!*T */
struct opt { u32 payload; _Bool is_null; };  /* Zig's ?usize */

extern void *grab(u32 n);

__attribute__((noinline)) static struct eu create(u32 n)
{
  struct eu r;
  void *p = grab(n);
  if (!p)
    return (struct eu){.payload = (void *)0xaaaaaaaa, .error = 8};
  r.payload = p;
  r.error = 0;
  return r;
}

__attribute__((noinline)) static struct opt find(const u32 *a, u32 n, u32 v)
{
  for (u32 i = 0; i < n; i++)
    if (a[i] == v)
      return (struct opt){i, 0};
  return (struct opt){0, 1};
}

int use_create(u32 n)
{
  struct eu t = create(n);
  if (t.error)
    return -(int)t.error;
  return (int)(u32)t.payload;
}

int use_find(const u32 *a, u32 n)
{
  struct opt o = find(a, n, 7);
  return o.is_null ? -1 : (int)o.payload;
}

/* Called, then its address taken: the address names find.aapcs. */
struct opt (*find_ptr(void))(const u32 *, u32, u32) { return find; }

/* Address taken first: stays AAPCS, no adapter. */
static struct opt early(u32 x);
struct opt (*const early_table[1])(u32) = {early};
static struct opt early(u32 x) { return (struct opt){x + 1, x == 0}; }
u32 use_early(u32 x) { return early(x).payload; }

/* An alias names the body, which stays AAPCS (a separate compile: create
 * is then not r0/r1). */
#ifdef REGPAIR_ALIAS
struct eu create_alias(u32 n) __attribute__((alias("create")));
#endif
