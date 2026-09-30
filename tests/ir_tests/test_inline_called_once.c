/* -finline-functions-called-once (on at -O1): every function body is saved
 * until the end of the TU, and a static function with exactly one call site
 * and no other reference is expanded at that call.  Covers callees defined
 * before and after their caller, chains of single calls, struct parameters and
 * returns, loops, early returns, callee locals named like caller locals, and
 * the functions that stay calls: two call sites, address taken, self
 * recursion, and a body that would force a frame pointer on its caller. */
#include <stdio.h>

struct pair
{
  int a, b;
};

struct big
{
  int v[6];
};

static int later_defined(int x);

static int square_plus(int x)
{
  return x * x + 1;
}

static int chain_c(int x)
{
  if (x < 0)
    return -1;
  return x + 3;
}

static int chain_b(int x)
{
  return chain_c(x * 2) + 1;
}

static int chain_a(int x)
{
  return chain_b(x + 1) * 10;
}

static void accumulate(int *acc, const int *vals, int n)
{
  for (int i = 0; i < n; i++)
  {
    if (vals[i] < 0)
      return;
    *acc += vals[i];
  }
}

static struct pair swap_pair(struct pair p)
{
  struct pair r = {p.b, p.a};
  return r;
}

static int big_sum(struct big b)
{
  int s = 0;
  for (int i = 0; i < 6; i++)
    s += b.v[i] * (i + 1);
  return s;
}

static int shadow_locals(int index)
{
  int total = 0;
  for (int k = 0; k < index; k++)
    total += k;
  return total;
}

static int called_twice(int x)
{
  return x - 7;
}

static int address_taken(int x)
{
  return x ^ 0x55;
}

static int fact(int n)
{
  return n <= 1 ? 1 : n * fact(n - 1);
}

__attribute__((noinline)) static int nonnull(void *p)
{
  return p != 0;
}

static int caller_known(void)
{
  return nonnull(__builtin_return_address(0));
}

static int (*volatile fp)(int);

int main(void)
{
  int acc = 0;
  int vals[] = {3, 4, -1, 100};
  struct pair p = {1, 2};
  struct big b = {{1, 2, 3, 4, 5, 6}};
  int total = 0;

  accumulate(&acc, vals, 4);
  p = swap_pair(p);
  for (int k = 0; k < 3; k++)
    total += k;
  fp = address_taken;

  printf("%d %d %d %d\n", square_plus(6), chain_a(2), later_defined(5), acc);
  printf("%d %d %d %d\n", p.a, p.b, big_sum(b), shadow_locals(5) + total);
  printf("%d %d %d %d\n", called_twice(10), called_twice(20), fp(0x0f), fact(5));
  printf("%d\n", caller_known());
  return 0;
}

static int later_defined(int x)
{
  int r = 0;
  while (x-- > 0)
    r += 2;
  return r;
}
