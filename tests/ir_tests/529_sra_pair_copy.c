/* Two more whole-object shapes SRA takes apart.
   A copy between two small locals over 16 bytes reaches the IR as an
   __aeabi_memmove4 call; when both objects are promotable and lay their
   fields out alike, the copy becomes copies between their field VARs (before,
   one of them kept its memory).
   A local initialized with constants -- `struct s t = {...}`, a memset and a
   run of constant stores that block_copy_init turns into one BLOCK_COPY from
   an image in .rodata -- gets its fields defined as those constants, a word
   holding an address as that address. */
#include <stdio.h>

struct rec
{
  unsigned a, b, c, d, e;
};

struct tab
{
  const char *name;
  unsigned lo, hi;
  unsigned char kind, flag;
  unsigned short id;
};

static const char greeting[] = "hello";

__attribute__((noinline)) unsigned sink(unsigned x)
{
  return x * 2654435761u;
}

__attribute__((noinline)) unsigned pair_copy(unsigned k)
{
  struct rec s, t;
  s.a = k;
  s.b = k + 1;
  s.c = k * 3;
  s.d = k ^ 0x55;
  s.e = sink(k);
  t = s; /* 20 bytes: a memmove4 between two promotable objects */
  s.a = 99; /* the source lives on */
  return t.a + t.b * 3 + t.c * 5 + t.d * 7 + t.e + s.a;
}

__attribute__((noinline)) unsigned image_init(unsigned k)
{
  struct tab t = {greeting, 10, 20, 3, 0, 0x1234};
  if (k & 1)
    t.lo = k;
  if (k & 2)
    t.flag = 1;
  unsigned r = t.name[k % 5] + t.lo + t.hi * 2 + t.kind + t.flag * 100 + t.id;
  return r;
}

__attribute__((noinline)) unsigned image_then_copy(unsigned k)
{
  struct rec s = {1, 2, 3, 4, 5};
  struct rec t;
  s.c += k;
  t = s;
  t.e = sink(t.a);
  return t.a + t.b + t.c + t.d + t.e;
}

int main(void)
{
  unsigned total = 0;
  for (unsigned k = 0; k < 6; k++)
  {
    unsigned a = pair_copy(k), b = image_init(k), c = image_then_copy(k);
    printf("k=%u a=%u b=%u c=%u\n", k, a, b, c);
    total += a ^ b ^ c;
  }
  printf("total=%u\n", total);
  return 0;
}
