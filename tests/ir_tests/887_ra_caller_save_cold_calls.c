/* ra:caller_save keeps a call-crossing value in a free caller-saved register
 * (r0-r3/ip) when every callee-saved one is taken, and the call lowering
 * stores it before and reloads it after each call it crosses.
 *
 * Each walker below reads eight loop invariants every iteration, so with the
 * table and bound r4-r11 are busy until the loop ends -- none of them outlives
 * the hot index, so none is an eviction victim -- and the index crosses a call
 * that only some iterations make.  The calls differ in what else touches the
 * caller-saved registers: a result in r0, a 64-bit result in r0:r1, a nested
 * call whose result is an argument, stack arguments, an indirect target.  A
 * value not saved, or a reload over the call's result, gives a wrong sum. */

typedef unsigned u32;
typedef unsigned long long u64;

static volatile u32 sink;

__attribute__((noinline)) static u32 cold(u32 x)
{
  sink += x;
  return x * 3u + 1u;
}

__attribute__((noinline)) static u64 cold64(u32 x)
{
  sink ^= x;
  return ((u64)x << 33) | (x ^ 0x55u);
}

__attribute__((noinline)) static u32 many(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f)
{
  sink += f;
  return a + 2u * b + 3u * c + 5u * d + 7u * e + 11u * f;
}

__attribute__((noinline)) static void cold_void(u32 x)
{
  sink -= x;
}

static u32 (*volatile p_cold)(u32) = cold;

#define LIVE8(p)                                                                                                       \
  u32 v0 = (p)[0] * 3u, v1 = (p)[1] ^ 0x1234u, v2 = (p)[2] + 77u, v3 = (p)[3] * 5u;                                    \
  u32 v4 = (p)[4] - 9u, v5 = (p)[5] * 11u, v6 = (p)[6] ^ 0xbeefu, v7 = (p)[7] + 1000u;
#define STEP(acc, i)                                                                                                   \
  ((acc) * 31u + tab[i] * ((i) + 1u) + ((v0 ^ (i)) + (v1 & (i)) + (v2 | (i)) + (v3 - (i))) +                            \
   ((v4 + (i)) ^ (v5 * (i))) + ((v6 >> ((i) & 7u)) ^ (v7 << ((i) & 3u))))

#define MARK 0xdeadu

__attribute__((noinline)) static u32 walk_result(const u32 *tab, u32 n, const u32 *p)
{
  LIVE8(p)
  u32 acc = 0;
  for (u32 i = 0; i < n; i++)
  {
    acc = STEP(acc, i);
    if (tab[i] == MARK)
      acc += cold(i + acc);
  }
  return acc;
}

__attribute__((noinline)) static u32 walk_wide(const u32 *tab, u32 n, const u32 *p)
{
  LIVE8(p)
  u32 acc = 0;
  for (u32 i = 0; i < n; i++)
  {
    acc = STEP(acc, i);
    if (tab[i] == MARK)
    {
      u64 w = cold64(i);
      acc += (u32)w + 3u * (u32)(w >> 32);
    }
  }
  return acc;
}

__attribute__((noinline)) static u32 walk_nested(const u32 *tab, u32 n, const u32 *p)
{
  LIVE8(p)
  u32 acc = 0;
  for (u32 i = 0; i < n; i++)
  {
    acc = STEP(acc, i);
    if (tab[i] == MARK)
      acc += many(i, acc, n, cold(i), tab[i] + i, 7u);
  }
  return acc;
}

__attribute__((noinline)) static u32 walk_void(const u32 *tab, u32 n, const u32 *p)
{
  LIVE8(p)
  u32 acc = 0;
  for (u32 i = 0; i < n; i++)
  {
    acc = STEP(acc, i);
    if (tab[i] == MARK)
      cold_void(acc ^ i);
  }
  return acc;
}

__attribute__((noinline)) static u32 walk_indirect(const u32 *tab, u32 n, const u32 *p, u32 (*f)(u32))
{
  LIVE8(p)
  u32 acc = 0;
  for (u32 i = 0; i < n; i++)
  {
    acc = STEP(acc, i);
    if (tab[i] == MARK)
      acc += f(acc - i);
  }
  return acc;
}

/* Two calls in the loop, one taken every few iterations, the other rarely. */
__attribute__((noinline)) static u32 walk_two(const u32 *tab, u32 n, const u32 *p)
{
  LIVE8(p)
  u32 acc = 0;
  for (u32 i = 0; i < n; i++)
  {
    acc = STEP(acc, i);
    if ((i & 7u) == 3u)
      cold_void(i);
    if (tab[i] == MARK)
      acc ^= cold(acc + i);
  }
  return acc;
}

int main(void)
{
  u32 tab[64], p[8];
  for (u32 i = 0; i < 64; i++)
    tab[i] = (i % 9u == 4u) ? MARK : i * 2654435761u;
  for (u32 i = 0; i < 8; i++)
    p[i] = 0x9e3779b9u * (i + 1u);

  if (walk_result(tab, 64, p) != 0x50b9b447u)
    return 1;
  if (walk_wide(tab, 64, p) != 0xcc99ffd0u)
    return 2;
  if (walk_nested(tab, 64, p) != 0xb74d4dd2u)
    return 3;
  if (walk_void(tab, 64, p) != 0x50e46d84u)
    return 4;
  if (walk_indirect(tab, 64, p, p_cold) != 0x03382e0bu)
    return 5;
  if (walk_two(tab, 64, p) != 0x1757357fu)
    return 6;
  if (walk_result(tab, 0, p) != 0x00000000u)
    return 7;
  return 0;
}
