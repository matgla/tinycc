/* Size peepholes that delete or merge instructions, and the shapes each one
   must leave alone.

   dead_def (ir/dead_def.c): a compound literal zero-fills a struct and then
   stores its fields; once SRA splits it into field TEMPs, the zero is
   overwritten before anything reads it (`movs r1,#0; ldr r1,[pc,#k]`).

   Narrow loads and their casts: `(signed char)u8` after LDRB, or
   `(unsigned short)s16` after LDRSH, still have to extend -- a rewrite
   that treated a narrow load's extension as settled by its operand types
   was tried and dropped (the code generator loaded an i8 with LDRB).

   CBZ bound (arm-thumb-gen.c rehearsal_max_shrink_between): the Zig error
   return `if (e.error) return e;` became CBZ once the fused branch stopped
   being counted twice.  Each chain must still take the right exit. */
#include <stdio.h>

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;

struct slice
{
  const u8 *ptr;
  unsigned len;
};

static const u8 hello[5] = {'h', 'e', 'l', 'l', 'o'};
static const u8 world[6] = {'w', 'o', 'r', 'l', 'd', '!'};

__attribute__((noinline)) unsigned hash(struct slice s)
{
  unsigned t = 7;
  for (unsigned i = 0; i < s.len; i++)
    t = t * 31u + s.ptr[i];
  return t;
}

__attribute__((noinline)) unsigned two(unsigned k)
{
  unsigned a = hash((struct slice){hello, 5});
  if (k)
    a ^= hash((struct slice){world, k});
  return a;
}

__attribute__((noinline)) int ext_sb(const u8 *p, int i) { return (s8)p[i]; }
__attribute__((noinline)) int ext_sh(const u8 *p, int i) { return (s16)p[i]; }
__attribute__((noinline)) int ext_ub(const u8 *p, int i) { return (u8)p[i]; }
__attribute__((noinline)) int ext_uh_of_s16(const s16 *q, int i) { return (u16)q[i]; }
__attribute__((noinline)) int ext_sb_of_s16(const s16 *q, int i) { return (s8)q[i]; }
__attribute__((noinline)) int ext_sh_of_s16(const s16 *q, int i) { return (s16)q[i]; }
__attribute__((noinline)) int ext_ub_of_u16(const u16 *q, int i) { return (u8)q[i]; }

struct eu
{
  u16 error;
};

__attribute__((noinline)) struct eu step(int x)
{
  struct eu r = {(u16)(x == 3 ? 7 : x == 5 ? 9 : 0)};
  return r;
}

__attribute__((noinline)) void note(int *n) { ++*n; }

__attribute__((noinline)) struct eu chain(int base, int *calls)
{
  struct eu t = step(base);
  if (t.error)
    return t;
  t = step(base + 1);
  if (t.error)
  {
    note(calls);
    return t;
  }
  t = step(base + 2);
  if (t.error)
    return (struct eu){(u16)(t.error + 100)};
  return (struct eu){0};
}

int main(void)
{
  printf("two %u %u %u\n", two(0), two(3), two(6));

  const u8 bytes[4] = {0x7f, 0x80, 0xff, 0x01};
  for (int i = 0; i < 4; i++)
    printf("u8[%d] sb=%d sh=%d ub=%d\n", i, ext_sb(bytes, i), ext_sh(bytes, i), ext_ub(bytes, i));

  const s16 halves[3] = {-2, 0x1234, -32768};
  for (int i = 0; i < 3; i++)
    printf("s16[%d] uh=%d sb=%d sh=%d\n", i, ext_uh_of_s16(halves, i), ext_sb_of_s16(halves, i),
           ext_sh_of_s16(halves, i));
  const u16 uh[2] = {0xff80, 0x017f};
  for (int i = 0; i < 2; i++)
    printf("u16[%d] ub=%d\n", i, ext_ub_of_u16(uh, i));

  for (int b = 0; b < 6; b++)
  {
    int calls = 0;
    struct eu e = chain(b, &calls);
    printf("chain(%d) = %u calls=%d\n", b, e.error, calls);
  }
  return 0;
}
