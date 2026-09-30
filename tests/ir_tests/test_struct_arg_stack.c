/* Structs passed by value from the frame load their words straight off sp/fp
 * instead of through a scratch base register.  Covers one to five words, a
 * struct parameter that arrived on the stack and is passed on, a function with
 * a frame pointer (VLA), a struct shorter than a word, and offsets beyond the
 * reach of the immediate forms (which must fall back). */
#include <stdio.h>
#include <stdint.h>

struct W1 { uint32_t a; };
struct W2 { uint32_t a, b; };
struct W3 { uint32_t a, b, c; };
struct W4 { uint32_t a, b, c, d; };
struct W5 { uint32_t a, b, c, d, e; };
struct B3 { uint8_t x, y, z; };

__attribute__((noinline)) static uint32_t s1(uint32_t k, struct W1 w) { return k * 7 + w.a; }
__attribute__((noinline)) static uint32_t s2(struct W2 w) { return w.a * 3 + w.b; }
__attribute__((noinline)) static uint32_t s3(struct W3 w) { return w.a + w.b * 5 + w.c * 11; }
__attribute__((noinline)) static uint32_t s4(struct W4 w) { return w.a + w.b * 2 + w.c * 3 + w.d * 4; }
__attribute__((noinline)) static uint32_t s5(struct W5 w) { return w.a + w.b + w.c + w.d * 9 + w.e * 13; }
__attribute__((noinline)) static uint32_t sb(struct B3 b) { return b.x * 10000u + b.y * 100u + b.z; }
__attribute__((noinline)) static struct W1 mk1(uint32_t k) { return (struct W1){k * 3 + 1}; }

__attribute__((noinline)) static uint32_t locals(uint32_t k)
{
  struct W1 t0, t1;
  t0 = mk1(k);
  t1 = t0;
  struct W2 w2 = {k, k + 1};
  struct W3 w3 = {k + 2, k + 3, k + 4};
  struct W4 w4 = {k, 2 * k, 3 * k, 4 * k};
  struct W5 w5 = {1, 2, 3, k, k + 5};
  struct B3 b = {(uint8_t)k, (uint8_t)(k + 1), (uint8_t)(k + 2)};
  return s1(k, t1) + s2(w2) + s3(w3) + s4(w4) + s5(w5) + sb(b);
}

/* q arrives on the stack (r0-r3 are taken) and is passed on. */
__attribute__((noinline)) static uint32_t pass_on(uint32_t a, uint32_t b, uint32_t c, uint32_t d, struct W3 q,
                                                  struct W1 r)
{
  return a + b + c + d + s3(q) + s1(a, r);
}

/* A VLA forces a frame pointer: fp-relative, possibly negative offsets. */
__attribute__((noinline)) static uint32_t with_vla(uint32_t n)
{
  volatile uint32_t v[n];
  for (uint32_t i = 0; i < n; i++)
    v[i] = i;
  struct W1 w1 = {n + v[0]};
  struct W3 w3 = {n, n * 2, n * 3 + v[n - 1]};
  return s1(n, w1) + s3(w3);
}

/* Structs above big arrays: offsets past 1020 and past 4095. */
__attribute__((noinline)) static uint32_t far(uint32_t k)
{
  struct W1 w1 = {k};
  struct W2 w2 = {k, k * 2};
  volatile uint8_t pad[1500];
  struct W3 w3 = {k, k + 1, k + 2};
  volatile uint8_t pad2[5000];
  struct W5 w5 = {k, 1, 2, 3, 4};
  pad[0] = (uint8_t)k;
  pad2[0] = pad[0];
  return s1(k, w1) + s2(w2) + s3(w3) + s5(w5) + pad2[0];
}

int main(void)
{
  printf("%u %u\n", locals(3), locals(1000));
  printf("%u\n", pass_on(1, 2, 3, 4, (struct W3){5, 6, 7}, (struct W1){8}));
  printf("%u %u\n", with_vla(1), with_vla(9));
  printf("%u %u\n", far(2), far(77));
  return 0;
}
