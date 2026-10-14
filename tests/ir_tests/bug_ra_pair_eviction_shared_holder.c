/* Linear-scan RA: the call-crossing 64-bit pair fallback evicts a callee-saved
 * victim without asking whether another interval still lives in its register.
 *
 * ra_alive_share puts a SECOND live holder on a register the allocator still
 * shows as owned by the first: here `x` (defined before the branch, read only
 * in the else arm, so it is dead across the whole then arm) owns one
 * callee-saved register while `b` (defined in the then arm, call-crossing, no
 * free callee-saved register left) borrows it.  Both are active at once.
 *
 * The 64-bit pair fallback ("evict single-INT victims to free the pair") then
 * picks `x` -- the cheapest victim, one use -- spills it and returns its
 * register to the free pool.  `b` is still living there, so the register is
 * handed on to the next claimant and `b` is overwritten:
 *
 *   test_leak: the pair cannot be completed (the second victim is hotter than
 *              the pair), so `l` is spilled -- but x's register has already
 *              been put back in the pool and the next call-crossing value `d`
 *              takes it, clobbering `b`.
 *   test_pair: `l` is hot enough to evict a second victim, so the pair is
 *              built from x's register directly and `l` overwrites `b`.
 *
 * Needs -O1+ (alive_share is gated on optimisation, and on the function having
 * no back edge, and on no coalesced interval anywhere in it) and exactly as
 * many call-crossing values as there are callee-saved registers, so the
 * values below are not decoration.  The fix: skip a victim whose register
 * another active interval still holds (ra_reg_has_other_holder), as the
 * single-INT spill scan already does.
 *
 * Expected output is from arm-none-eabi-gcc -O0 and -O2.
 */
#include <stdio.h>

static volatile unsigned vg;

__attribute__((noinline)) static unsigned ext(unsigned a)
{
  vg = vg * 31u + a;
  return vg;
}

__attribute__((noinline)) static unsigned long long ext64(unsigned a)
{
  vg = vg * 17u + a;
  return ((unsigned long long)vg << 20) ^ (vg * 3u);
}

__attribute__((noinline)) static unsigned test_leak(unsigned c)
{
  unsigned v0 = ext(1);
  unsigned v1 = ext(2);
  unsigned v2 = ext(3);
  unsigned v3 = ext(4);
  unsigned v4 = ext(5);
  unsigned v5 = ext(6);
  unsigned x = ext(100);
  unsigned r = 0;
  if (c & 1) {
    unsigned b = ext(200);
    unsigned long long l = ext64(b);
    unsigned d = ext(300);
    ext(5);
    r += d * 11u + (d >> 3);
    r += b;
    r += (unsigned)l + (unsigned)(l >> 32);
    r ^= (unsigned)(l >> 7);
    r += (unsigned)l * 3u;
    r += b * 5u + c;
    r ^= v0;
    r ^= v1;
    r ^= v2;
    r ^= v3;
    r ^= v4;
    r ^= v5;
    r += v0 * 3u + v1 * 4u + v2 * 5u + v3 * 6u + v4 * 7u + v5 * 8u;
    return r;
  }
  r += x + c * 7u;
  r ^= v0;
  r ^= v1;
  r ^= v2;
  r ^= v3;
  r ^= v4;
  r ^= v5;
  r += v0 * 3u + v1 * 4u + v2 * 5u + v3 * 6u + v4 * 7u + v5 * 8u;
  return r + 1u;
}

__attribute__((noinline)) static unsigned test_pair(unsigned c)
{
  unsigned v0 = ext(1);
  unsigned v1 = ext(2);
  unsigned v2 = ext(3);
  unsigned v3 = ext(4);
  unsigned v4 = ext(5);
  unsigned v5 = ext(6);
  unsigned x = ext(100);
  unsigned r = 0;
  if (c & 1) {
    unsigned b = ext(200);
    unsigned long long l = ext64(b);
    ext(5);
    r += b;
    r += (unsigned)l + (unsigned)(l >> 32);
    r ^= (unsigned)(l >> 7);
    r += (unsigned)l * 3u;
    r ^= (unsigned)(l >> 11);
    r += (unsigned)(l >> 13) + (unsigned)(l >> 40);
    r ^= (unsigned)l;
    r += (unsigned)(l >> 21) * 5u;
    r += (unsigned)(l >> 33) * 7u;
    r += b * 5u + c;
    r ^= v0;
    r ^= v1;
    r ^= v2;
    r ^= v3;
    r ^= v4;
    r ^= v5;
    r += v0 * 3u + v1 * 4u + v2 * 5u + v3 * 6u + v4 * 7u + v5 * 8u;
    return r;
  }
  r += x + c * 7u;
  r ^= v0;
  r ^= v1;
  r ^= v2;
  r ^= v3;
  r ^= v4;
  r ^= v5;
  r += v0 * 3u + v1 * 4u + v2 * 5u + v3 * 6u + v4 * 7u + v5 * 8u;
  return r + 1u;
}

int main(void)
{
  unsigned a = test_leak(1);
  unsigned b = test_leak(0);
  unsigned c = test_pair(1);
  unsigned d = test_pair(0);
  printf("leak_then=%u leak_else=%u\n", a, b);
  printf("pair_then=%u pair_else=%u\n", c, d);
  return 0;
}
