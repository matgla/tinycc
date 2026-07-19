/* Guard: gen_opic reverses constant-on-the-left relational compares.
 *
 * `K < x` is emitted as `x > K` so the constant encodes into the CMP immediate
 * instead of being materialized into a register first.  Reversal (not negation)
 * is the transformation, so it must hold exactly for every predicate and both
 * signednesses — including at the boundaries where a negation-based rewrite
 * would differ.
 *
 * Each check compares the constant-on-the-left form against the hand-written
 * swapped form; any mismatch in the reversal table shows up as a differing
 * count.  Signed values straddle 0 and INT_MIN/INT_MAX; unsigned values
 * straddle the wrap point, which is where a signed/unsigned mix-up would show.
 */
#include <stdio.h>

#define INT_MIN_ (-2147483647 - 1)
#define INT_MAX_ 2147483647

__attribute__((noinline)) int s_lt(int x) { return 5 < x; }
__attribute__((noinline)) int s_gt(int x) { return 5 > x; }
__attribute__((noinline)) int s_le(int x) { return 5 <= x; }
__attribute__((noinline)) int s_ge(int x) { return 5 >= x; }

__attribute__((noinline)) int u_lt(unsigned x) { return 5u < x; }
__attribute__((noinline)) int u_gt(unsigned x) { return 5u > x; }
__attribute__((noinline)) int u_le(unsigned x) { return 5u <= x; }
__attribute__((noinline)) int u_ge(unsigned x) { return 5u >= x; }

__attribute__((noinline)) int l_lt(long long x) { return 5LL < x; }
__attribute__((noinline)) int l_ge(long long x) { return 5LL >= x; }
__attribute__((noinline)) int ul_lt(unsigned long long x) { return 5ULL < x; }

/* extreme constants: the reversal must not disturb the immediate range checks */
__attribute__((noinline)) int s_min(int x) { return INT_MIN_ < x; }
__attribute__((noinline)) int s_max(int x) { return INT_MAX_ >= x; }
__attribute__((noinline)) int u_top(unsigned x) { return 0xffffffffu > x; }

int main(void)
{
  static const int sv[] = {INT_MIN_, -6, -5, -1, 0, 1, 4, 5, 6, 7, INT_MAX_};
  static const unsigned uv[] = {0u, 1u, 4u, 5u, 6u, 0x7fffffffu, 0x80000000u, 0xffffffffu};
  int i, acc = 0;

  for (i = 0; i < (int)(sizeof sv / sizeof sv[0]); i++)
  {
    int x = sv[i];
    acc += s_lt(x) != (x > 5);
    acc += s_gt(x) != (x < 5);
    acc += s_le(x) != (x >= 5);
    acc += s_ge(x) != (x <= 5);
    acc += l_lt(x) != ((long long)x > 5LL);
    acc += l_ge(x) != ((long long)x <= 5LL);
    acc += s_min(x) != (x > INT_MIN_);
    acc += s_max(x) != (x <= INT_MAX_);
  }
  for (i = 0; i < (int)(sizeof uv / sizeof uv[0]); i++)
  {
    unsigned x = uv[i];
    acc += u_lt(x) != (x > 5u);
    acc += u_gt(x) != (x < 5u);
    acc += u_le(x) != (x >= 5u);
    acc += u_ge(x) != (x <= 5u);
    acc += ul_lt(x) != ((unsigned long long)x > 5ULL);
    acc += u_top(x) != (x < 0xffffffffu);
  }

  printf("mismatches=%d\n", acc);
  /* a couple of concrete values so a wholesale predicate inversion cannot pass */
  printf("s_lt(4)=%d s_lt(6)=%d s_ge(4)=%d s_ge(6)=%d\n", s_lt(4), s_lt(6), s_ge(4), s_ge(6));
  printf("u_gt(0)=%d u_gt(9)=%d u_top(0xffffffff)=%d\n", u_gt(0u), u_gt(9u), u_top(0xffffffffu));
  return 0;
}
