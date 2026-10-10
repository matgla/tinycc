/* ra:struct_arg_split.  A by-value struct argument whose words are stored into
 * a frame slot right before the call is passed as one scalar word per
 * register / stack slot instead.  Every case here must come out the same as
 * with the pass off: struct sizes of 1-4 words, the argument straddling r3 and
 * the stack, arguments after it, the slot read again after the call, a slot
 * whose address is taken, partial / out-of-order / conditional stores, a slot
 * reused by two calls, structs that are NOT plain word sequences (HFA, 8-byte
 * alignment, 6 bytes), two struct arguments, nested calls and calls through a
 * pointer. */
#include <stdio.h>
#include <stdint.h>

typedef uint32_t u32;

struct W1 { u32 a; };
struct W2 { u32 a, b; };
struct W3 { u32 a, b, c; };
struct W4 { u32 a, b, c, d; };
struct SL { uint8_t *p; u32 n; };
struct FI { float f; u32 i; };
struct HF { float x, y; };
struct LL { long long v; u32 k; };
struct B6 { uint8_t b[6]; };
struct SLC { const char *p; u32 n; };

#define NOINL __attribute__((noinline)) static

NOINL u32 c1(u32 k, struct W1 w) { return k * 7 + w.a; }
NOINL u32 c2(u32 k, struct W2 w) { return k * 7 + w.a * 3 + w.b; }
NOINL u32 c3(u32 k, struct W3 w) { return k * 7 + w.a + w.b * 5 + w.c * 11; }
NOINL u32 c4(u32 k, struct W4 w) { return k * 7 + w.a + w.b * 2 + w.c * 3 + w.d * 4; }

/* arg registers consumed before the struct: 1 (r0), 2, 3 (struct starts in r3) */
NOINL u32 d2_1(u32 x, struct W2 w) { return x + w.a * 3 + w.b; }
NOINL u32 d2_2(u32 x, u32 y, struct W2 w) { return x + y * 2 + w.a * 3 + w.b; }
NOINL u32 d2_3(u32 x, u32 y, u32 z, struct W2 w) { return x + y * 2 + z * 4 + w.a * 3 + w.b; }
NOINL u32 d3_3(u32 x, u32 y, u32 z, struct W3 w) { return x + y * 2 + z * 4 + w.a + w.b * 5 + w.c * 11; }
NOINL u32 d4_3(u32 x, u32 y, u32 z, struct W4 w) { return x + y * 2 + z * 4 + w.a + w.b * 2 + w.c * 3 + w.d * 4; }
NOINL u32 d4_4(u32 x, u32 y, u32 z, u32 t, struct W4 w) { return x + y * 2 + z * 4 + t * 8 + w.a + w.b * 2 + w.c * 3 + w.d * 4; }
/* arguments after the struct */
NOINL u32 e3(struct W3 w, u32 x, u32 y) { return w.a + w.b * 5 + w.c * 11 + x * 13 + y * 17; }
NOINL u32 e4_mid(u32 x, struct W4 w, u32 y, u32 z) { return x + w.a + w.b * 2 + w.c * 3 + w.d * 4 + y * 13 + z * 17; }
NOINL u32 e2_split(u32 x, u32 y, u32 z, struct W2 w, u32 t) { return x + y * 2 + z * 4 + w.a * 3 + w.b + t * 19; }
NOINL u32 two(struct W2 a, struct W3 b) { return a.a * 3 + a.b + b.a + b.b * 5 + b.c * 11; }
static uint8_t gbuf[8];
NOINL u32 slc(void *self, struct SL s) { return (u32)(uintptr_t)self * 3 + (u32)(s.p - gbuf) * 5 + s.n; }
NOINL u32 fi(u32 k, struct FI v) { return k + (u32)(v.f * 4.0f) + v.i * 3; }
NOINL u32 hf(u32 k, struct HF v) { return k + (u32)(v.x * 4.0f) + (u32)(v.y * 8.0f); }
NOINL u32 ll(u32 k, struct LL v) { return k + (u32)v.v * 3 + v.k * 5; }
NOINL u32 b6(u32 k, struct B6 v) { return k + v.b[0] + v.b[1] * 2 + v.b[2] * 3 + v.b[3] * 4 + v.b[4] * 5 + v.b[5] * 6; }
NOINL u32 lc(u32 k, struct SLC s) { return k + (u32)s.p[1] * 3 + s.n; }
NOINL struct W3 sret3(u32 k, struct W2 w) { return (struct W3){k, w.a, w.b}; }
NOINL u32 vsum(u32 n, ...);
NOINL u32 touch(struct W2 *p) { p->a += 1; return p->b; }

volatile u32 seed = 3;
volatile u32 sink;

/* The plain cases: build from scalars, call. */
NOINL u32 plain(u32 k)
{
  u32 s = 0;
  struct W1 w1 = {k + 1};
  struct W2 w2 = {k + 2, k * 3};
  struct W3 w3 = {k + 3, k * 5, k ^ 7};
  struct W4 w4 = {k + 4, k * 7, k ^ 9, k - 11};
  s += c1(k, w1) + c2(k, w2) + c3(k, w3) + c4(k, w4);
  s += d2_1(k, w2) + d2_2(k, k + 1, w2) + d2_3(k, k + 1, k + 2, w2);
  s += d3_3(k, k + 1, k + 2, w3) + d4_3(k, k + 1, k + 2, w4) + d4_4(k, k + 1, k + 2, k + 3, w4);
  s += e3(w3, k, k + 1) + e4_mid(k, w4, k + 1, k + 2) + e2_split(k, k + 1, k + 2, w2, k + 3);
  s += two(w2, w3);
  return s;
}

/* The same value passed twice: the slot is read after the first call. */
NOINL u32 reread(u32 k)
{
  struct W3 w = {k + 3, k * 5, k ^ 7};
  u32 a = c3(k, w);
  w.b += 1; /* a later store, a later read */
  u32 b = c3(k + 1, w);
  return a * 31 + b + w.c;
}

/* The slot's address is taken: it can be changed behind the call's back. */
NOINL u32 addr_taken(u32 k)
{
  struct W2 w = {k + 2, k * 3};
  u32 r = touch(&w);
  return r + d2_3(k, k, k, w);
}

/* A partial overwrite of a word, and stores in the opposite order. */
NOINL u32 partial(u32 k)
{
  struct W3 w;
  w.c = k ^ 7;
  w.b = k * 5;
  w.a = k + 3;
  u32 r = c3(k, w);
  ((uint8_t *)&w)[1] = (uint8_t)(k + 9); /* byte store into word a */
  return r + c3(k + 1, w);
}

/* Words stored on two paths, and one stored before the branch. */
NOINL u32 cond(u32 k)
{
  struct W3 w;
  w.a = k + 3;
  if (k & 1)
  {
    w.b = k * 5;
    w.c = 11;
  }
  else
  {
    w.b = k * 6;
    w.c = k ^ 7;
  }
  return c3(k, w);
}

/* One slot reused by successive calls (the front end does that), in a loop. */
NOINL u32 loopy(u32 k)
{
  u32 s = 0;
  for (u32 i = 0; i < k; i++)
  {
    struct W2 w = {i + k, i * 3};
    s += d2_3(i, k, s, w);
    struct W3 v = {i, i + 1, k};
    s += d3_3(k, i, s, v);
  }
  return s;
}

/* A stored value redefined between the store and the call. */
NOINL u32 redef(u32 k)
{
  u32 a = k + 1, b = k * 3;
  struct W2 w = {a, b};
  a += 100;
  b ^= 0x55;
  return d2_3(a, b, k, w);
}

/* Not word sequences: float member, 8-byte alignment, HFA, 6 bytes. */
NOINL u32 odd(u32 k)
{
  struct FI f = {(float)k + 0.5f, k * 2};
  struct HF h = {(float)k + 0.25f, (float)k * 2.0f};
  struct LL l = {(long long)k * 3, k + 1};
  struct B6 b = {{(uint8_t)k, (uint8_t)(k + 1), (uint8_t)(k + 2), (uint8_t)(k + 3), (uint8_t)(k + 4), (uint8_t)(k + 5)}};
  return fi(k, f) + hf(k, h) + ll(k, l) + b6(k, b);
}

NOINL u32 sl_args(u32 k)
{
  struct SL s = {gbuf + (k & 3), k + 1};
  return slc((void *)(uintptr_t)(k * 2), s);
}

/* A string literal's pointer and length: the pointer is a symbol's address. */
NOINL u32 lits(u32 k)
{
  struct SLC a = {"hello", 5};
  struct SLC b = {(k & 1) ? "xyz" : "pqr", k};
  return lc(k, a) + lc(k + 1, b);
}

/* The call's result is a struct returned through a hidden pointer. */
NOINL u32 sret(u32 k)
{
  struct W2 w = {k + 1, k * 3};
  struct W3 r = sret3(k, w);
  return r.a + r.b * 2 + r.c * 3;
}

/* A struct argument evaluated before a nested call that has its own. */
NOINL u32 nested(u32 k)
{
  struct W2 w = {k + 1, k * 3};
  struct W2 v = {k + 2, k * 5};
  return d2_3(k, d2_1(k + 1, v), k + 2, w);
}

/* Through a function pointer. */
NOINL u32 indirect(u32 k, u32 (*f)(u32, u32, u32, struct W3))
{
  struct W3 w = {k + 1, k * 3, k ^ 5};
  return f(k, k + 1, k + 2, w);
}

NOINL u32 vsum(u32 n, ...)
{
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  u32 s = 0;
  for (u32 i = 0; i < n; i++)
    s = s * 3 + __builtin_va_arg(ap, u32);
  __builtin_va_end(ap);
  return s;
}

/* Words of a struct handed to a variadic callee one by one. */
NOINL u32 variadic(u32 k)
{
  struct W3 w = {k + 1, k * 3, k ^ 5};
  return vsum(4, k, w.a, w.b, w.c);
}

int main(void)
{
  u32 k = seed;
  printf("%u %u\n", plain(k), plain(k + 100));
  printf("%u %u\n", reread(k), reread(k + 1));
  printf("%u %u\n", addr_taken(k), addr_taken(k + 1));
  printf("%u %u\n", partial(k), partial(k + 7));
  printf("%u %u %u\n", cond(k), cond(k + 1), cond(k + 2));
  printf("%u %u\n", loopy(k), loopy(k + 2));
  printf("%u %u\n", redef(k), redef(k + 5));
  printf("%u %u\n", odd(k), odd(k + 4));
  printf("%u %u\n", sl_args(k), sl_args(k + 1));
  printf("%u %u\n", lits(k), lits(k + 1));
  printf("%u %u\n", sret(k), sret(k + 1));
  printf("%u %u\n", nested(k), nested(k + 1));
  printf("%u %u\n", indirect(k, d3_3), indirect(k + 1, d3_3));
  printf("%u %u\n", variadic(k), variadic(k + 1));
  return 0;
}
