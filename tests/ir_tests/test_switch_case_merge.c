/* Switch case bodies that differ only in their own case constant share one
 * copy that reads the selector instead (region_merge, case constants): tags
 * written back as the value switched on, from 0 and from a non-zero first
 * case; and the shapes that must stay apart -- a constant that is not the
 * case value, an entry also reached by a goto, a selector changed inside the
 * body, a 64-bit selector, negative case values. */
#include <stdio.h>
#include <stdint.h>

struct R { uint8_t tag; uint32_t payload; };

__attribute__((noinline)) static uint32_t f(uint32_t k) { return k * 7 + 1; }

/* Cases from 0: the table indexes with the selector itself. */
__attribute__((noinline)) static void from0(struct R *r, uint32_t x)
{
  switch (x)
  {
  case 0: r->payload = f(3); r->tag = 0; break;
  case 1: r->payload = f(3); r->tag = 1; break;
  case 2: r->payload = f(3); r->tag = 2; break;
  case 3: r->payload = f(3); r->tag = 3; break;
  case 4: r->payload = f(3); r->tag = 4; break;
  case 5: r->payload = f(3); r->tag = 5; break;
  default: r->payload = 0; r->tag = 99; break;
  }
}

/* Cases from 3: the table indexes with x - 3. */
__attribute__((noinline)) static void from3(struct R *r, uint32_t x)
{
  switch (x)
  {
  case 3: r->payload = f(x); r->tag = 3; break;
  case 4: r->payload = f(x); r->tag = 4; break;
  case 5: r->payload = f(x); r->tag = 5; break;
  case 6: r->payload = f(x); r->tag = 6; break;
  case 7: r->payload = f(x); r->tag = 7; break;
  case 8: r->payload = f(x); r->tag = 8; break;
  default: r->payload = 1; r->tag = 98; break;
  }
}

/* The constant is k + 1, not the case value. */
__attribute__((noinline)) static void plus1(struct R *r, uint32_t x)
{
  switch (x)
  {
  case 0: r->payload = f(5); r->tag = 1; break;
  case 1: r->payload = f(5); r->tag = 2; break;
  case 2: r->payload = f(5); r->tag = 3; break;
  case 3: r->payload = f(5); r->tag = 4; break;
  case 4: r->payload = f(5); r->tag = 5; break;
  default: r->payload = 2; r->tag = 97; break;
  }
}

/* Case 2's body is also reached by a goto, with a selector that is not 2. */
__attribute__((noinline)) static void jumped(struct R *r, uint32_t x, int skip)
{
  if (skip)
    goto two;
  switch (x)
  {
  case 0: r->payload = f(6); r->tag = 0; break;
  case 1: r->payload = f(6); r->tag = 1; break;
  case 2:
  two:
    r->payload = f(6); r->tag = 2; break;
  case 3: r->payload = f(6); r->tag = 3; break;
  case 4: r->payload = f(6); r->tag = 4; break;
  default: r->payload = 3; r->tag = 96; break;
  }
}

/* The selector variable is overwritten before the constant is written. */
__attribute__((noinline)) static void changed(struct R *r, uint32_t x)
{
  switch (x)
  {
  case 0: x = f(x); r->payload = x; r->tag = 0; break;
  case 1: x = f(x); r->payload = x; r->tag = 1; break;
  case 2: x = f(x); r->payload = x; r->tag = 2; break;
  case 3: x = f(x); r->payload = x; r->tag = 3; break;
  case 4: x = f(x); r->payload = x; r->tag = 4; break;
  default: r->payload = 4; r->tag = 95; break;
  }
  r->payload += x;
}

/* A 64-bit selector. */
__attribute__((noinline)) static void wide(struct R *r, uint64_t x)
{
  switch (x)
  {
  case 0: r->payload = f(8); r->tag = 0; break;
  case 1: r->payload = f(8); r->tag = 1; break;
  case 2: r->payload = f(8); r->tag = 2; break;
  case 3: r->payload = f(8); r->tag = 3; break;
  case 4: r->payload = f(8); r->tag = 4; break;
  default: r->payload = 5; r->tag = 94; break;
  }
}

/* Negative case values through a signed narrow selector. */
__attribute__((noinline)) static int neg(int8_t x)
{
  volatile int out;
  switch (x)
  {
  case -3: f(1); out = -3; break;
  case -2: f(1); out = -2; break;
  case -1: f(1); out = -1; break;
  case 0: f(1); out = 0; break;
  case 1: f(1); out = 1; break;
  case 2: f(1); out = 2; break;
  default: out = 50; break;
  }
  return out;
}

/* Zig's shape: the tag stored into a result union through a pointer, the
 * selector a narrow field loaded once. */
struct U { uint8_t tag; uint32_t a, b; };
__attribute__((noinline)) static void zigish(struct U *out, const struct U *in)
{
  switch (in->tag)
  {
  case 0: out->a = f(in->a); out->b = in->b; out->tag = 0; break;
  case 1: out->a = f(in->a); out->b = in->b; out->tag = 1; break;
  case 2: out->a = f(in->a); out->b = in->b; out->tag = 2; break;
  case 3: out->a = f(in->a); out->b = in->b; out->tag = 3; break;
  case 4: out->a = f(in->a); out->b = in->b; out->tag = 4; break;
  case 5: out->a = f(in->a); out->b = in->b; out->tag = 5; break;
  case 6: out->a = 0; out->b = 0; out->tag = 6; break;
  default: out->a = 1; out->b = 1; out->tag = 7; break;
  }
}

/* The selector spilled to a frame slot by the calls around it. */
__attribute__((noinline)) static uint32_t spilled(struct U *out, const struct U *in, uint32_t p, uint32_t q,
                                                 uint32_t r, uint32_t s, uint32_t t, uint32_t u, uint32_t v,
                                                 uint32_t w)
{
  uint8_t tag = in->tag;
  uint32_t x0 = f(p), x1 = f(q), x2 = f(r), x3 = f(s), x4 = f(t), x5 = f(u), x6 = f(v), x7 = f(w);
  switch (tag)
  {
  case 0: out->a = f(in->a); out->b = x0; out->tag = 0; break;
  case 1: out->a = f(in->a); out->b = x0; out->tag = 1; break;
  case 2: out->a = f(in->a); out->b = x0; out->tag = 2; break;
  case 3: out->a = f(in->a); out->b = x0; out->tag = 3; break;
  case 4: out->a = f(in->a); out->b = x0; out->tag = 4; break;
  default: out->a = 1; out->b = 1; out->tag = 7; break;
  }
  return x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7 + tag;
}

int main(void)
{
  struct R r;
  uint32_t sum = 0;
  for (uint32_t x = 0; x < 8; x++)
  {
    from0(&r, x);
    printf("%u:%u ", r.tag, r.payload);
    sum += r.tag;
  }
  printf("\n");
  for (uint32_t x = 1; x < 11; x++)
  {
    from3(&r, x);
    printf("%u:%u ", r.tag, r.payload);
  }
  printf("\n");
  for (uint32_t x = 0; x < 6; x++)
  {
    plus1(&r, x);
    printf("%u ", r.tag);
  }
  printf("\n");
  for (uint32_t x = 0; x < 6; x++)
  {
    jumped(&r, x, 0);
    printf("%u ", r.tag);
    jumped(&r, x, 1);
    printf("%u ", r.tag);
  }
  printf("\n");
  for (uint32_t x = 0; x < 6; x++)
  {
    changed(&r, x);
    printf("%u:%u ", r.tag, r.payload);
  }
  printf("\n");
  for (uint64_t x = 0; x < 6; x++)
  {
    wide(&r, x + (x == 5 ? 0x100000000ull : 0));
    printf("%u ", r.tag);
  }
  printf("\n");
  for (int x = -4; x < 4; x++)
    printf("%d ", neg((int8_t)x));
  printf("\n");
  for (uint8_t t = 0; t < 9; t++)
  {
    struct U in = {t, t * 10u, t * 100u}, o;
    zigish(&o, &in);
    printf("%u:%u:%u ", o.tag, o.a, o.b);
  }
  printf("\n");
  for (uint8_t t = 0; t < 7; t++)
  {
    struct U in = {t, t * 10u, 0}, o;
    uint32_t v = spilled(&o, &in, t, 1, 2, 3, 4, 5, 6, 7);
    printf("%u:%u:%u:%u ", o.tag, o.a, o.b, v);
  }
  printf("\n");
  printf(sum == 99 + 99 + 15 ? "PASS\n" : "FAIL\n");
  return 0;
}
