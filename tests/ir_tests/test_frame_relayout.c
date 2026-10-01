/* Frame objects the optimizer leaves unreferenced are dropped and the live
 * ones packed towards the frame top (frame.c / tcc_ir_frame_relayout).  Dead
 * aggregates sit between live ones; live objects are reached through escaping
 * pointers, one-past-end pointers, struct copies, sret and inline return slots,
 * and a frame far past the 16-bit [sp, #imm] reach.  Every result is checked. */
#include <stdio.h>
#include <string.h>

struct pair
{
  int a, b;
};
struct big
{
  int v[40];
};

__attribute__((noinline)) static int sum(const int *p, const int *end)
{
  int s = 0;
  while (p != end)
    s += *p++;
  return s;
}

__attribute__((noinline)) static int sum_n(const int *p, int n)
{
  return sum(p, p + n);
}

__attribute__((noinline)) static void fill(int *p, int n, int k)
{
  for (int i = 0; i < n; i++)
    p[i] = k * i + 1;
}

__attribute__((noinline)) static struct big make_big(int k)
{
  struct big b;
  for (int i = 0; i < 40; i++)
    b.v[i] = i ^ k;
  return b;
}

static inline struct pair swap(struct pair p)
{
  struct pair r = {p.b, p.a};
  return r;
}

/* A never-referenced array between two live ones. */
__attribute__((noinline)) static int dead_between(int n)
{
  int hi[8];
  char unused[600] __attribute__((unused));
  int lo[8];
  for (int i = 0; i < 8; i++)
  {
    hi[i] = i + n;
    lo[i] = 100 * i;
  }
  return sum_n(hi, 8) + sum_n(lo, 8);
}

/* Dead stores only: the zeroed struct is never read. */
__attribute__((noinline)) static int dead_zeroed(int n)
{
  struct big scratch;
  memset(&scratch, 0, sizeof scratch);
  int live[4];
  fill(live, 4, n);
  return sum_n(live, 4);
}

/* Struct copies, an sret temporary and an inlined struct return, all moved up
 * over a dead odd-sized array. */
__attribute__((noinline)) static int copies(int k)
{
  char pad[99] __attribute__((unused));
  struct pair p = {k, 2 * k};
  struct pair q = p;
  struct pair r = swap(q);
  struct big b = make_big(k);
  struct big c = b;
  return r.a - r.b + c.v[3] + c.v[39] + b.v[0];
}

/* Far offsets: a big live array below several dead ones, spills below that. */
__attribute__((noinline)) static unsigned far_frame(unsigned s)
{
  char dead1[1500] __attribute__((unused));
  int keep[300];
  char dead2[3000] __attribute__((unused));
  int near_[4];
  fill(keep, 300, (int)s);
  fill(near_, 4, 3);
  unsigned a0 = s + keep[1], a1 = s + keep[299], a2 = s ^ keep[7], a3 = s * 3u, a4 = s + 5, a5 = s + 6;
  unsigned a6 = s + 7, a7 = s + 8, a8 = s + 9, a9 = s + 10, a10 = s + 11, a11 = s + 12;
  unsigned t = (unsigned)sum_n(keep, 300) + (unsigned)sum_n(near_, 4);
  return t + a0 * a11 + a1 * a10 + a2 * a9 + a3 * a8 + a4 * a7 + a5 * a6;
}

__attribute__((noinline)) static void fill64(long long *p, int n, long long k)
{
  for (int i = 0; i < n; i++)
    p[i] = k * i + 1;
}

/* An 8-byte-aligned object moved over a 13-byte hole stays 8-byte aligned. */
__attribute__((noinline)) static long long aligned_move(long long k)
{
  char pad[13] __attribute__((unused));
  long long w[3];
  fill64(w, 3, k);
  return w[0] + w[1] * w[2] + (long long)((unsigned)(unsigned long)&w[0] & 7u);
}

/* The by-value parameters' homes are untracked allocations the prologue stores
 * to: they stay put while the array below the dead one moves up to them. */
__attribute__((noinline)) static int by_value(struct pair p, struct big b, int k)
{
  char dead[40] __attribute__((unused));
  int arr[4];
  fill(arr, 4, k);
  return p.a * 10 + p.b + sum_n(arr, 4) + b.v[0] + b.v[39];
}

/* A walk ends at the lowest array's one-past-end pointer, which lies in the
 * dead array above it. */
__attribute__((noinline)) static int adjacent(void)
{
  int x[2] = {1, 2};
  char gap[64] __attribute__((unused));
  int y[2] = {3, 4};
  const int *e = y + 2;
  int s = 0;
  for (const int *p = y; p != e; p++)
    s += *p;
  return s + x[0] + x[1];
}

int main(void)
{
  printf("%d\n", dead_between(5));
  printf("%d\n", dead_zeroed(7));
  printf("%d\n", copies(9));
  printf("%u\n", far_frame(11));
  printf("%d\n", adjacent());
  printf("%lld\n", aligned_move(1000000007LL));
  printf("%d\n", by_value((struct pair){3, 4}, make_big(5), 6));
  return 0;
}
