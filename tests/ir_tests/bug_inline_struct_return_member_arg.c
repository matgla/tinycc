/*
 *  TCC IR - a struct returned by an inlined call, whose member is an inlined call's argument
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* inner(outer(...).member) with both inlined.  dead_lea_store did not count
 * `V <-- StackLoc[x] [STORE]` as a read of the slot, so the writes to the returned
 * struct were dropped.  ssa:stack_addr_simplify followed `T <-- U***DEREF*** ADD #k`
 * as if U's address, not the pointer loaded through it, were the base, so a pointer
 * kept in a local struct read the struct's own frame. */

extern int printf(const char *, ...);

typedef struct __attribute__((packed)) Packed
{
  int word;
  int imm;
  unsigned char flags;
} Packed;

typedef struct Plain
{
  int word;
  int imm;
  int more;
} Plain;

typedef struct Ctx
{
  Packed *packed;
  Plain *plain;
  int count;
} Ctx;

static const Packed no_packed = {-1, 0, 0};
static const Plain no_plain = {-1, 0, 0};

Packed get_packed(const Ctx *c, int i)
{
  const int k = i + 3;
  if (k >= 0 && k < c->count)
    return c->packed[k];
  return no_packed;
}

Plain get_plain(const Ctx *c, int i)
{
  const int k = i + 3;
  if (k >= 0 && k < c->count)
    return c->plain[k];
  return no_plain;
}

int decode(int w)
{
  const unsigned type = (unsigned)w >> 28, pos = (unsigned)w & 0x1FFFFu;
  if (type == 0)
    return -1;
  return (int)((type << 28) | pos);
}

int packed_word(const Ctx *c, int i)
{
  return decode(get_packed(c, i).word);
}

int plain_word(const Ctx *c, int i)
{
  return decode(get_plain(c, i).word);
}

int packed_second_member(const Ctx *c, int i)
{
  return decode(get_packed(c, i).imm);
}

/* The same value through a named copy, which never lost it: pins the neighbour. */
int named_copy(const Ctx *c, int i)
{
  Packed p = get_packed(c, i);
  return decode(p.word);
}

/* Only the second fix: a pointer in a local struct, offset by a constant. */
int __attribute__((noinline)) pointer_in_local(void)
{
  int a[4] = {10, 20, 30, 40};
  struct
  {
    int *p;
    int pad;
  } s = {a, 0};
  int *volatile keep = a;
  (void)keep;
  return *(int *)((char *)(&s)->p + 8);
}

int main(void)
{
  Packed packed[5] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0x20000007, 0x30000001, 2}, {0x10000003, 5, 4}};
  Plain plain[5] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0x20000017, 1, 2}, {0x10000013, 3, 4}};
  Ctx c = {packed, plain, 5};
  printf("%x %x %x\n", packed_word(&c, 0), packed_word(&c, 1), packed_word(&c, 5));
  printf("%x %x %x\n", plain_word(&c, 0), plain_word(&c, 1), plain_word(&c, 5));
  printf("%x %x\n", packed_second_member(&c, 0), packed_second_member(&c, 1));
  printf("%x %x\n", named_copy(&c, 0), named_copy(&c, 1));
  printf("%d\n", pointer_in_local());
  return 0;
}
