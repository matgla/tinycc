/* Small struct locals touched only a word at a time live in registers (sra).
 * The shapes it meets: one-word wrappers passed and returned by value,
 * multi-word values copied between locals and carried round a loop, merged
 * from two paths.  Objects that must stay in memory -- a float member passed
 * by value, a byte field, a volatile struct, one whose address escapes -- keep
 * their exact behaviour. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

struct Idx { uint32_t v; };
struct Pair { uint32_t a, b; };
struct Quad { uint32_t a, b, c, d; };
struct F1 { float f; };
struct Tag { uint8_t t; uint8_t pad[3]; };

__attribute__((noinline)) static struct Idx next(struct Idx i) { return (struct Idx){i.v * 3 + 1}; }
__attribute__((noinline)) static uint32_t get(struct Idx i) { return i.v; }
__attribute__((noinline)) static uint32_t sum_pair(struct Pair p) { return p.a * 7 + p.b; }
__attribute__((noinline)) static uint32_t sum_quad(struct Quad q) { return q.a + q.b * 2 + q.c * 3 + q.d * 4; }
__attribute__((noinline)) static float half(struct F1 x) { return x.f * 0.5f; }
__attribute__((noinline)) static uint32_t tagv(struct Tag t) { return t.t; }
__attribute__((noinline)) static void poke(uint32_t *p) { *p += 1000; }

/* One-word values through calls, changed between them. */
__attribute__((noinline)) static uint32_t chain(uint32_t k)
{
  struct Idx a, b;
  a.v = k;
  b = next(a);
  a = b;
  a.v += 2;
  b = next(a);
  return get(a) * 100 + get(b);
}

/* A value carried round a loop, merged from two paths each time. */
__attribute__((noinline)) static uint32_t loop(uint32_t n)
{
  struct Pair p = {1, 2};
  struct Pair t;
  for (uint32_t i = 0; i < n; i++)
  {
    if (i & 1)
    {
      t.a = p.b;
      t.b = p.a + i;
    }
    else
    {
      t.a = p.a * 2;
      t.b = p.b;
    }
    p = t;
  }
  return sum_pair(p);
}

/* Four words, copied whole and field by field. */
__attribute__((noinline)) static uint32_t quad(uint32_t k)
{
  struct Quad q = {k, k + 1, k + 2, k + 3};
  struct Quad r = q;
  r.c = q.a + q.d;
  struct Quad s;
  s = r;
  s.a ^= 5;
  return sum_quad(s) + sum_quad(q);
}

/* Must stay in memory: a float member passed by value (a VFP register under
 * the hard-float ABI), a byte field, a volatile struct, an escaping address. */
__attribute__((noinline)) static uint32_t stay(uint32_t k)
{
  struct F1 f;
  f.f = (float)k;
  struct Tag t;
  memset(&t, 0, sizeof t);
  t.t = (uint8_t)(k + 7);
  volatile struct Idx v;
  v.v = k;
  v.v = v.v + 1;
  struct Idx e = {k};
  poke(&e.v);
  return (uint32_t)(half(f) * 4.0f) + tagv(t) * 10 + v.v * 100 + e.v;
}

int main(void)
{
  printf("%u %u\n", chain(1), chain(40));
  printf("%u %u %u\n", loop(0), loop(5), loop(12));
  printf("%u %u\n", quad(3), quad(100));
  printf("%u %u\n", stay(2), stay(250));
  return 0;
}
