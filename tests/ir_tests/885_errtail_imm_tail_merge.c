/* Tails that differ in one constant (or one symbol address) are merged: the
 * shared copy takes the value from a register each path loads just before it
 * branches there.  That register must not be one the shared tail or the code
 * after it uses, a path must deliver its own constant -- also when two paths
 * agree on it -- and values live into the tail must survive. */
#include <stdio.h>
#include <stdint.h>

typedef struct { uint64_t lo, hi; } u128;
struct eu { u128 payload; uint16_t error; };

__attribute__((noinline)) uint8_t step(int x, int fail_at) { return (uint8_t)(x == fail_at ? 1 : 0); }

#define UNDEF_ERR(code) return (struct eu){.error = (code), .payload = {0xaaaaaaaaaaaaaaaaull, 0x2aaaaaaaaaaull}}

__attribute__((noinline)) struct eu chain(int fail_at)
{
  if (step(1, fail_at))
    UNDEF_ERR(111);
  if (step(2, fail_at))
    UNDEF_ERR(112);
  if (step(3, fail_at))
    UNDEF_ERR(112);
  if (step(4, fail_at))
    UNDEF_ERR(113);
  if (step(5, fail_at))
    UNDEF_ERR(114);
  struct eu r;
  r.error = 0;
  r.payload.lo = (uint64_t)fail_at;
  r.payload.hi = 7;
  return r;
}

/* every argument register is live into the tails */
__attribute__((noinline)) unsigned wide_live(unsigned a, unsigned b, unsigned c, unsigned d, unsigned *p)
{
  if (a == 1)
  {
    p[0] = a; p[1] = b; p[2] = c; p[3] = d; p[4] = 0x11;
    return a ^ b ^ c ^ d;
  }
  if (a == 2)
  {
    p[0] = a; p[1] = b; p[2] = c; p[3] = d; p[4] = 0x22;
    return a ^ b ^ c ^ d;
  }
  if (a == 3)
  {
    p[0] = a; p[1] = b; p[2] = c; p[3] = d; p[4] = 0x33;
    return a ^ b ^ c ^ d;
  }
  p[0] = 0;
  return 0;
}

/* values computed in several registers flow into the shared stores and out of it */
__attribute__((noinline)) unsigned mixed_live(unsigned a, unsigned b, int k, unsigned *p)
{
  unsigned x = a * 3u, y = b + 5u, z = a ^ b;
  if (k == 1)
  {
    p[0] = x; p[1] = y; p[2] = z; p[3] = 0x101;
    return x + y + z;
  }
  if (k == 2)
  {
    p[0] = x; p[1] = y; p[2] = z; p[3] = 0x202;
    return x + y + z;
  }
  if (k == 3)
  {
    p[0] = x; p[1] = y; p[2] = z; p[3] = 0x303;
    return x + y + z;
  }
  return 0;
}

/* different symbol addresses */
static const char *last_msg;
__attribute__((noinline)) int say(int k, uint32_t *p)
{
  if (k == 1)
  {
    p[0] = 5; p[1] = 6;
    last_msg = "one";
    return 1;
  }
  if (k == 2)
  {
    p[0] = 5; p[1] = 6;
    last_msg = "two";
    return 1;
  }
  if (k == 3)
  {
    p[0] = 5; p[1] = 6;
    last_msg = "three";
    return 1;
  }
  return 0;
}

/* narrow stores */
__attribute__((noinline)) int narrow(int k, uint16_t *h, uint8_t *b)
{
  if (k == 1)
  {
    h[0] = 0x1234; b[0] = 1; b[1] = 2;
    return 10;
  }
  if (k == 2)
  {
    h[0] = 0x4321; b[0] = 1; b[1] = 2;
    return 10;
  }
  if (k == 3)
  {
    h[0] = 0x5555; b[0] = 1; b[1] = 2;
    return 10;
  }
  return 0;
}

/* exits from a loop whose counters are live in registers */
__attribute__((noinline)) int loop_exit(const int *v, int n, uint32_t *out)
{
  int sum = 0;
  for (int i = 0; i < n; i++)
  {
    if (v[i] < 0)
    {
      out[0] = (uint32_t)sum; out[1] = (uint32_t)i; out[2] = 0xdead; out[3] = 0x70;
      return -1;
    }
    if (v[i] == 0)
    {
      out[0] = (uint32_t)sum; out[1] = (uint32_t)i; out[2] = 0xdead; out[3] = 0x71;
      return -1;
    }
    if (v[i] > 1000)
    {
      out[0] = (uint32_t)sum; out[1] = (uint32_t)i; out[2] = 0xdead; out[3] = 0x72;
      return -1;
    }
    sum += v[i];
  }
  out[3] = 0;
  return sum;
}

int main(void)
{
  for (int f = 0; f <= 6; f++)
  {
    struct eu r = chain(f);
    printf("chain(%d) error=%u lo=%llx hi=%llx\n", f, r.error, (unsigned long long)r.payload.lo,
           (unsigned long long)r.payload.hi);
  }
  for (unsigned a = 0; a <= 4; a++)
  {
    unsigned p[5] = {9, 9, 9, 9, 9};
    unsigned r = wide_live(a, 10, 20, 30, p);
    printf("wide_live(%u)=%u p=%x %x %x %x %x\n", a, r, p[0], p[1], p[2], p[3], p[4]);
  }
  for (int k = 0; k <= 4; k++)
  {
    unsigned p[4] = {9, 9, 9, 9};
    unsigned r = mixed_live(7, 8, k, p);
    printf("mixed_live(%d)=%u p=%x %x %x %x\n", k, r, p[0], p[1], p[2], p[3]);
  }
  for (int k = 0; k <= 4; k++)
  {
    uint32_t p[2] = {0, 0};
    last_msg = "none";
    int r = say(k, p);
    printf("say(%d)=%d %s %u %u\n", k, r, last_msg, p[0], p[1]);
  }
  for (int k = 0; k <= 4; k++)
  {
    uint16_t h[1] = {0};
    uint8_t b[2] = {0, 0};
    int r = narrow(k, h, b);
    printf("narrow(%d)=%d %x %u %u\n", k, r, h[0], b[0], b[1]);
  }
  int cases[4][4] = {{1, 2, 3, 4}, {1, 2, -3, 4}, {5, 0, 3, 4}, {1, 2000, 3, 4}};
  for (int c = 0; c < 4; c++)
  {
    uint32_t out[4] = {1, 1, 1, 1};
    int r = loop_exit(cases[c], 4, out);
    printf("loop_exit(%d)=%d out=%u %u %x %x\n", c, r, out[0], out[1], out[2], out[3]);
  }
  return 0;
}
