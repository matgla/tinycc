/* Graph-coalescer coverage: diamond-arm phi copies and 64-bit pair classes.
 *
 * Guards three admissions added together:
 *  - diamond-arm copy with a source dead at every sibling def of the dest
 *    (interval-containment rule) — BOTH arms of a value-select diamond must
 *    merge, and the class inherits the r0 RETURNVALUE preference;
 *  - LLONG-LLONG pair-copy classes (latch copy of a 64-bit accumulator,
 *    64-bit switch-merge into a shared use);
 *  - 32-bit coalescing inside functions that also hold LLONG values (the
 *    old whole-function LLONG bail).
 *
 * Miscompiled coalescing shows up as a clobbered phi arm (wrong sat/range
 * result), a corrupted pair half (wrong acc/sw hex words), or a wrong IV
 * after sequential loops (mx). */

extern int printf(const char *, ...);
extern void exit(int);

unsigned sat_add(unsigned i)
{
  unsigned ret = i + 1;
  if (ret < i)
    ret = i;
  return ret;
}

int in_range(long long x)
{
  return x > 0xFFFFFFFFLL || x < -0x80000000LL;
}

unsigned long long mul_acc(unsigned long long a, unsigned long long b, int k)
{
  unsigned long long acc = a;
  while (k--)
    acc = acc * 3ULL + b;
  return acc;
}

unsigned long long sw_merge(unsigned long long x, int n, unsigned long long *out)
{
  unsigned long long r = x;
  switch (n)
  {
  case 1:
    r = x << 1;
    break;
  case 2:
    r = x << 7;
    break;
  case 3:
    r = x >> 3;
    break;
  case 4:
    r = x + 5;
    break;
  }
  *out = r ^ 1;
  return r;
}

unsigned mixed(unsigned n)
{
  unsigned long long t = n;
  unsigned s = 0;
  unsigned i;
  for (i = 0; i < n; i++)
    s += i;
  for (; i < n + 8; i++)
    s ^= i;
  t = t * 10ULL + s;
  return (unsigned)(t >> 3);
}

int main(void)
{
  unsigned long long m, o, r;
  printf("sat=%u %u\n", sat_add(5u), sat_add(0xFFFFFFFFu));
  printf("rng=%d %d %d\n", in_range(0), in_range(0x100000000LL),
         in_range(-0x80000001LL));
  m = mul_acc(7ULL, 0x100000001ULL, 5);
  printf("acc=%08x%08x\n", (unsigned)(m >> 32), (unsigned)m);
  o = 0;
  r = sw_merge(0x123456789ALL, 2, &o);
  printf("sw=%08x%08x o=%08x%08x\n", (unsigned)(r >> 32), (unsigned)r,
         (unsigned)(o >> 32), (unsigned)o);
  r = sw_merge(0x123456789ALL, 3, &o);
  printf("sw3=%08x%08x\n", (unsigned)(r >> 32), (unsigned)r);
  printf("mx=%u\n", mixed(10));
  printf("OK\n");
  exit(0);
  return 0;
}
