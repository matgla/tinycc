/* ra:evict_pair: a 64-bit value held in a register pair across a whole loop --
 * read once before it and once after, like the kernel loader's replay
 * timestamp -- may be the victim a hot single value evicts.  The pair goes to
 * an 8-byte slot, both registers come free, and the loop's values use them.
 * Every walker keeps eight loop invariants, the bound and the table live with
 * calls in the loop, so the callee-saved registers run out; the pair must come
 * back intact after the loop, whichever register took its place. */

typedef unsigned u32;
typedef unsigned long long u64;

static volatile u32 sink;
static volatile u64 clock_src = 0x0123456789abcdefull;

__attribute__((noinline)) static u64 now(void)
{
  clock_src = clock_src * 6364136223846793005ull + 1442695040888963407ull;
  return clock_src;
}

__attribute__((noinline)) static u32 cold(u32 x)
{
  sink += x;
  return x * 3u + 1u;
}

#define LIVE8(p)                                                                                                       \
  u32 v0 = (p)[0] * 3u, v1 = (p)[1] ^ 0x1234u, v2 = (p)[2] + 77u, v3 = (p)[3] * 5u;                                    \
  u32 v4 = (p)[4] - 9u, v5 = (p)[5] * 11u, v6 = (p)[6] ^ 0xbeefu, v7 = (p)[7] + 1000u;
#define STEP(acc, i)                                                                                                   \
  ((acc) * 31u + tab[i] * ((i) + 1u) + ((v0 ^ (i)) + (v1 & (i)) + (v2 | (i)) + (v3 - (i))) +                            \
   ((v4 + (i)) ^ (v5 * (i))) + ((v6 >> ((i) & 7u)) ^ (v7 << ((i) & 3u))))

/* the pair is read after the loop */
__attribute__((noinline)) static u64 timed_walk(const u32 *tab, u32 n, const u32 *p)
{
  u64 t0 = now();
  LIVE8(p)
  u32 acc = 0;
  for (u32 i = 0; i < n; i++)
  {
    acc = STEP(acc, i);
    if (tab[i] == 0xdeadu)
      acc += cold(i + acc);
  }
  u64 t1 = now();
  return (t1 - t0) ^ ((u64)acc << 17) ^ t0;
}

/* the pair is an argument of the call after the loop */
__attribute__((noinline)) static u64 mix(u64 a, u32 b)
{
  return a * 0x9e3779b97f4a7c15ull + b;
}

__attribute__((noinline)) static u64 timed_walk_arg(const u32 *tab, u32 n, const u32 *p)
{
  u64 t0 = now();
  LIVE8(p)
  u32 acc = 0;
  for (u32 i = 0; i < n; i++)
  {
    acc = STEP(acc, i);
    if (tab[i] == 0xdeadu)
      acc ^= cold(acc - i);
  }
  return mix(t0, acc);
}

int main(void)
{
  u32 tab[64], p[8];
  for (u32 i = 0; i < 64; i++)
    tab[i] = (i % 9u == 4u) ? 0xdeadu : i * 2654435761u;
  for (u32 i = 0; i < 8; i++)
    p[i] = 0x9e3779b9u * (i + 1u);
  if (timed_walk(tab, 64, p) != 0xb1d63c67f0b45c35ull)
    return 1;
  if (timed_walk_arg(tab, 64, p) != 0xba194b9e2c4f0fa7ull)
    return 2;
  if (timed_walk(tab, 0, p) != 0x01e898d76ad2cf00ull)
    return 3;
  return 0;
}
