/* Stores through an incoming pointer that a later store in the same straight
 * line overwrites entirely are dead -- the zero fill of Zig's
 * `return .{ .error = X, .payload = undefined }` through the sret pointer is
 * written twice.  The pointer may reach the same bytes by different vregs
 * (p, p+4, an alias), widths differ (a 64-bit store covers two 32-bit ones),
 * and every read, call or branch between the two stores keeps the first one:
 * these are the cases that must stay correct whatever gets removed. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct { uint64_t lo, hi; } u128;
struct eu { u128 payload; uint16_t error; };

static int log_count;
__attribute__((noinline)) uint8_t step(int x) { return (uint8_t)(x == 2 ? 3 : 0); }
__attribute__((noinline)) void note(int *p) { log_count += p[0]; }

/* the sret pointer is written, overwritten by wider stores, then the tail */
__attribute__((noinline)) struct eu first(int k)
{
  if (step(k))
    return (struct eu){.error = 111, .payload = {0xaaaaaaaaaaaaaaaaull, 0x2aaaaaaaaaaull}};
  struct eu r;
  r.error = 0;
  r.payload.lo = (uint64_t)k;
  r.payload.hi = 0;
  return r;
}

/* word stores overwritten by a 64-bit store */
__attribute__((noinline)) void wide(uint32_t *p)
{
  p[0] = 1;
  p[1] = 2;
  *(uint64_t *)p = 0x1111111122222222ull;
}

/* a 64-bit store only partly overwritten by a word store: the low word stays */
__attribute__((noinline)) void narrow_after_wide(uint32_t *p)
{
  *(uint64_t *)p = 0x1111111122222222ull;
  p[1] = 7;
}

/* a read through another pointer between the stores sees the first value */
__attribute__((noinline)) int read_between(int *p, int *q)
{
  p[0] = 1;
  int a = q[0];
  p[0] = 2;
  return a;
}

/* a call between the stores may read through the pointer */
__attribute__((noinline)) void call_between(int *p)
{
  p[0] = 5;
  note(p);
  p[0] = 6;
}

/* an alias of the same bytes */
__attribute__((noinline)) void alias_overwrite(int *p)
{
  int *q = p + 1;
  p[1] = 11;
  q[0] = 12;
  p[2] = 13;
}

/* a byte store does not cover a word, a word store covers the byte */
__attribute__((noinline)) void byte_word(uint8_t *p)
{
  *(uint32_t *)p = 0x01020304u;
  p[2] = 0xee;
  *(uint32_t *)(p + 4) = 0x0a0b0c0du;
  p[5] = 0x55;
  *(uint32_t *)(p + 4) = 0x00112233u;
}

/* volatile stores all happen */
__attribute__((noinline)) void vol(volatile int *p)
{
  p[0] = 1;
  p[0] = 2;
}

/* the pointer moves between the stores: p[0] below is the old p[1] */
__attribute__((noinline)) void param_bump(uint32_t *p)
{
  p[0] = 1;
  p++;
  p[0] = 2;
  p[-1] = 3;
}

/* a branch between the stores: the first one is live on the else path */
__attribute__((noinline)) void branchy(uint32_t *p, int c)
{
  p[0] = 1;
  if (c)
    p[0] = 2;
  else
    p[1] = 3;
}

/* a block copy between the stores reads the first */
__attribute__((noinline)) void copy_between(uint32_t *p, uint32_t *q)
{
  p[0] = 7;
  memcpy(q, p, 4);
  p[0] = 8;
}

/* an index loop: each iteration overwrites what the previous one stored */
__attribute__((noinline)) void loop_fill(uint32_t *p, int n)
{
  for (int i = 0; i < n; i++)
  {
    p[0] = (uint32_t)i;
    p[1] = 100u + (uint32_t)i;
    p[0] = 50u + (uint32_t)i;
  }
}

int main(void)
{
  for (int k = 1; k <= 3; k++)
  {
    struct eu r = first(k);
    printf("first(%d) error=%u lo=%llx hi=%llx\n", k, r.error, (unsigned long long)r.payload.lo,
           (unsigned long long)r.payload.hi);
  }
  uint32_t w[3] = {9, 9, 9};
  wide(w);
  printf("wide %x %x %x\n", w[0], w[1], w[2]);
  uint32_t n[3] = {9, 9, 9};
  narrow_after_wide(n);
  printf("narrow %x %x %x\n", n[0], n[1], n[2]);
  int a[3] = {20, 21, 22};
  printf("read_between alias=%d", read_between(a, a));
  printf(" distinct=%d", read_between(a + 1, a));
  printf(" a=%d %d %d\n", a[0], a[1], a[2]);
  int c[1] = {0};
  call_between(c);
  printf("call_between log=%d c=%d\n", log_count, c[0]);
  int al[4] = {0, 0, 0, 0};
  alias_overwrite(al);
  printf("alias %d %d %d %d\n", al[0], al[1], al[2], al[3]);
  uint8_t b[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  byte_word(b);
  printf("byte_word %02x %02x %02x %02x %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
  uint32_t pb[3] = {9, 9, 9};
  param_bump(pb);
  printf("param_bump %u %u %u\n", pb[0], pb[1], pb[2]);
  for (int c = 0; c <= 1; c++)
  {
    uint32_t br[2] = {9, 9};
    branchy(br, c);
    printf("branchy(%d) %u %u\n", c, br[0], br[1]);
  }
  uint32_t cp[1] = {9}, cq[1] = {0};
  copy_between(cp, cq);
  printf("copy_between %u %u\n", cp[0], cq[0]);
  uint32_t lf[2] = {9, 9};
  loop_fill(lf, 3);
  printf("loop_fill %u %u\n", lf[0], lf[1]);
  volatile int v = 0;
  vol(&v);
  printf("vol %d\n", v);
  return 0;
}
