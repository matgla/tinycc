/* ra:reload_elim deny-by-default kill set: SWITCH_TABLE/SWITCH_LOAD (clobber
 * r12 and the dispatch registers), BUILTIN_APPLY and LONGJMP must each forget
 * "slot S holds registers rA:rB".  Same union-pun shape as
 * bug_ra_reload_elim_block_copy_implicit_calls: store a double, run the op,
 * reload the bits, compare against an independently computed value. */
#include <setjmp.h>
#include <stdio.h>

typedef union
{
  double d;
  long long u;
} U;

static volatile int vk = 3;
static volatile double va = 1.5;

__attribute__((noinline)) long long switch_between(double a, int k)
{
  U x;
  int t;
  x.d = a;
  switch (k)
  {
  case 0: t = 11; break;
  case 1: t = 22; break;
  case 2: t = 33; break;
  case 3: t = 44; break;
  case 4: t = 55; break;
  case 5: t = 66; break;
  default: t = 77; break;
  }
  long long r = x.u;
  return r + t;
}

static int twice(int v) { return v * 2; }

__attribute__((noinline)) long long apply_between(double a, int k)
{
  U x;
  x.d = a;
  void *args = __builtin_apply_args();
  void *res = __builtin_apply((void (*)())twice, args, 64);
  long long r = x.u;
  (void)res;
  return r + k;
}

static jmp_buf jb;

__attribute__((noinline)) void do_jump(int v)
{
  longjmp(jb, v);
}

__attribute__((noinline)) long long longjmp_between(double a, int k)
{
  U x;
  volatile long long keep;
  x.d = a;
  keep = x.u;
  int s = setjmp(jb);
  if (s == 0)
    do_jump(k);
  long long r = x.u;
  return r + s + (keep != 0);
}

static long long bits(double d)
{
  U x;
  x.d = d;
  return x.u;
}

static int check(const char *name, long long got, long long want)
{
  if (got != want)
  {
    printf("%s: got %lld want %lld\n", name, got, want);
    return 1;
  }
  printf("%s ok\n", name);
  return 0;
}

int main(void)
{
  int bad = 0;
  double a = va;
  int k = vk;
  long long ab = bits(a);

  bad += check("switch_between", switch_between(a, k), ab + 44);
  bad += check("switch_between_default", switch_between(a, 9), ab + 77);
  bad += check("longjmp_between", longjmp_between(a, k), ab + 3 + 1);
  bad += check("apply_between", apply_between(a, k), ab + k);
  return bad ? 1 : 0;
}
