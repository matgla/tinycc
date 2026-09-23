/* Identical code folding: a static function whose body matches one already
 * generated is dropped and its symbol points at the survivor.  What must NOT
 * fold: bodies differing in a constant, in the function they call, in the
 * global they touch, or in a literal-pool value; and any function whose
 * address is taken -- by `&`, by naming it, or by passing it on -- since a
 * program can compare those.  A recursive body calls itself, which is not the
 * same as calling another function. */
#include <stdint.h>
#include <stdio.h>

static int g_one = 11;
static int g_two = 22;

__attribute__((noinline)) static int mul3(const int *p, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += p[i] * 3;
  return s;
}

/* identical to mul3 */
__attribute__((noinline)) static int mul3_again(const int *p, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += p[i] * 3;
  return s;
}

/* differs by a constant */
__attribute__((noinline)) static int mul5(const int *p, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += p[i] * 5;
  return s;
}

/* differs by a literal-pool constant */
__attribute__((noinline)) static int big_a(int x)
{
  return x * 3 + 0x12345678;
}

__attribute__((noinline)) static int big_b(int x)
{
  return x * 3 + 0x7654321;
}

/* differ by the global they read */
__attribute__((noinline)) static int read_one(void)
{
  return g_one * 2;
}

__attribute__((noinline)) static int read_two(void)
{
  return g_two * 2;
}

/* differ by the function they call */
__attribute__((noinline)) static int via_one(const int *p, int n)
{
  return mul3(p, n) + 1;
}

__attribute__((noinline)) static int via_five(const int *p, int n)
{
  return mul5(p, n) + 1;
}

/* identical recursion: each calls itself */
__attribute__((noinline)) static int fact_a(int n)
{
  return n <= 1 ? 1 : n * fact_a(n - 1);
}

__attribute__((noinline)) static int fact_b(int n)
{
  return n <= 1 ? 1 : n * fact_b(n - 1);
}

/* identical, but their addresses are compared */
__attribute__((noinline)) static int key_a(int x)
{
  return x ^ 0x5a;
}

__attribute__((noinline)) static int key_b(int x)
{
  return x ^ 0x5a;
}

/* identical, one named (decays to a pointer) without & */
__attribute__((noinline)) static int named_a(int x)
{
  return x + 77;
}

__attribute__((noinline)) static int named_b(int x)
{
  return x + 77;
}

__attribute__((noinline)) static int apply(int (*f)(int), int x)
{
  return f(x);
}

static int (*volatile ka)(int) = key_a;
static int (*volatile kb)(int) = key_b;

int main(void)
{
  int v[5] = {1, 2, 3, 4, 5};
  printf("%d %d %d\n", mul3(v, 5), mul3_again(v, 5), mul5(v, 5));
  printf("%d %d\n", big_a(10), big_b(10));
  printf("%d %d\n", read_one(), read_two());
  printf("%d %d\n", via_one(v, 5), via_five(v, 5));
  printf("%d %d\n", fact_a(5), fact_b(6));
  printf("%d %d %d\n", ka(7), kb(7), ka == kb);
  printf("%d %d %d\n", apply(named_a, 1), apply(named_b, 2), named_a == named_b);
  return 0;
}
