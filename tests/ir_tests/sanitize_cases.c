/* -fsanitize=... / -ftrapv cases (test_sanitize.py): argv[1] names one; the
 * program prints "go <case>", does it, and prints "ok <case> <value>" -- or
 * never gets there, the check's UDF ending in the HardFault handler (in
 * handler mode: the runtime's report, then "ok" or its abort).  Every
 * operand comes from a volatile, so nothing folds at compile time. */
#include <limits.h>
#include <stdio.h>
#include <string.h>

volatile int vi_max = INT_MAX, vi_min = INT_MIN, vi_one = 1, vi_m1 = -1, vi_zero = 0, vi_two = 2, vi_big = 65536;
volatile int vi_31 = 31, vi_32 = 32, vi_neg = -3, vi_five = 5, vi_four = 4, vi_three = 3;
volatile long long vl_max = LLONG_MAX, vl_min = LLONG_MIN, vl_one = 1, vl_m1 = -1, vl_big = 1LL << 32;
volatile unsigned vu_max = UINT_MAX;
volatile short vs_max = SHRT_MAX;
int *volatile vp_null;
char buf[16] __attribute__((aligned(8)));
char *volatile vp_buf = buf;

struct S
{
  int n;
  int tail[1];
};

static int arr[4] = {1, 2, 3, 4};

static int vla_sum(int n)
{
  int v[n];
  for (int i = 0; i < n; i++)
    v[i] = i;
  return n > 0 ? v[n - 1] : 0;
}

static int inl(int a, int b) { return a + b; }

int main(int argc, char **argv)
{
  const char *c = argc > 1 ? argv[1] : "none";
  long long r = 0;
  printf("go %s\n", c);
  if (!strcmp(c, "add"))
    r = vi_max + vi_one;
  else if (!strcmp(c, "add_ok"))
    r = vi_max + vi_m1;
  else if (!strcmp(c, "add_const"))
    r = vi_max + 1;
  else if (!strcmp(c, "add_const_ok"))
    r = vi_min + 1;
  else if (!strcmp(c, "sub"))
    r = vi_min - vi_one;
  else if (!strcmp(c, "sub_const"))
    r = vi_min - 1;
  else if (!strcmp(c, "sub_ok"))
    r = vi_zero - vi_max;
  else if (!strcmp(c, "mul"))
    r = vi_big * vi_big;
  else if (!strcmp(c, "mul_ok"))
    r = vi_big * vi_m1;
  else if (!strcmp(c, "mul_min"))
    r = vi_min * vi_m1;
  else if (!strcmp(c, "neg"))
    r = -vi_min;
  else if (!strcmp(c, "neg_ok"))
    r = -vi_max;
  else if (!strcmp(c, "inc"))
  {
    int x = vi_max;
    x++;
    r = x;
  }
  else if (!strcmp(c, "compound"))
  {
    int x = vi_max;
    x += vi_two;
    r = x;
  }
  else if (!strcmp(c, "inline"))
    r = inl(vi_max, vi_one) + inl(vi_one, vi_one);
  else if (!strcmp(c, "ladd"))
    r = vl_max + vl_one;
  else if (!strcmp(c, "lsub"))
    r = vl_min - vl_one;
  else if (!strcmp(c, "lmul"))
    r = vl_big * vl_big;
  else if (!strcmp(c, "lmul_ok"))
    r = vl_big * (vl_big >> 2) + vl_m1 * vl_big;
  else if (!strcmp(c, "lmul_shift_ok"))
    r = (vl_big >> 2) * vl_big - vl_big * (vl_big >> 2);
  else if (!strcmp(c, "negret_ok"))
  {
    /* a negation feeding the return register (the ra return-share fix) */
    unsigned short e = (unsigned short)vi_five;
    r = e ? -(int)e : vi_zero;
  }
  else if (!strcmp(c, "lmul_min"))
    r = vl_min * vl_m1;
  else if (!strcmp(c, "uadd_ok"))
    r = vu_max + 1u;
  else if (!strcmp(c, "short_ok"))
  {
    short s = vs_max;
    s++;
    r = vs_max + vs_max;
  }
  else if (!strcmp(c, "div0"))
    r = vi_one / vi_zero;
  else if (!strcmp(c, "mod0"))
    r = vi_one % vi_zero;
  else if (!strcmp(c, "udiv0"))
    r = (unsigned)vi_one / (unsigned)vi_zero;
  else if (!strcmp(c, "divmin"))
    r = vi_min / vi_m1;
  else if (!strcmp(c, "div_ok"))
    r = vi_min / vi_two;
  else if (!strcmp(c, "ldiv0"))
    r = vl_one / (vl_one - 1);
  else if (!strcmp(c, "shl_big"))
    r = vi_one << vi_32;
  else if (!strcmp(c, "shl_neg"))
    r = vi_one << vi_neg;
  else if (!strcmp(c, "shr_big"))
    r = vi_one >> vi_32;
  else if (!strcmp(c, "shl_sign"))
    r = vi_one << vi_31;
  else if (!strcmp(c, "shl_negbase"))
    r = vi_m1 << vi_one;
  else if (!strcmp(c, "shl_ok"))
    r = vi_one << vi_five;
  else if (!strcmp(c, "ushl_ok"))
    r = (unsigned)vi_one << vi_31;
  else if (!strcmp(c, "null"))
    r = *vp_null;
  else if (!strcmp(c, "null_store"))
    *vp_null = 1;
  else if (!strcmp(c, "align"))
    r = *(int *)(vp_buf + 1);
  else if (!strcmp(c, "align_ok"))
    r = *(int *)(vp_buf + 4);
  else if (!strcmp(c, "vla"))
    r = vla_sum(vi_zero);
  else if (!strcmp(c, "vla_neg"))
    r = vla_sum(vi_neg);
  else if (!strcmp(c, "vla_ok"))
    r = vla_sum(vi_five);
  else if (!strcmp(c, "bounds"))
    r = arr[vi_four];
  else if (!strcmp(c, "bounds_neg"))
    r = arr[vi_m1];
  else if (!strcmp(c, "bounds_store"))
    arr[vi_four] = 7;
  else if (!strcmp(c, "bounds_ok"))
    r = arr[vi_three];
  else if (!strcmp(c, "bounds_addr_ok"))
    r = &arr[vi_four] - arr;
  else if (!strcmp(c, "add_loop"))
  {
    /* the same place three times: a recovering handler reports it once */
    for (int k = 0; k < 3; k++)
      r += vi_max + vi_one;
  }
  else if (!strcmp(c, "bounds_flex_ok"))
  {
    static int mem[4] = {9, 8, 7, 6};
    struct S *s = (struct S *)mem;
    r = s->tail[vi_two];
  }
  printf("ok %s %lld\n", c, r);
  return 0;
}
