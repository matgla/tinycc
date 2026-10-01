/* Text/data separation: R9 holds the data base.  A call to a static function
 * -- this module's own even when its definition comes further down -- skips the
 * post-call R9 reload.  The callees defined later read and write globals,
 * recurse, and call each other, so a wrong R9 after any call shows up as a
 * wrong global access. */
#include <stdio.h>

static int counter;
static int table[8] = {3, 1, 4, 1, 5, 9, 2, 6};
static const char *const names[3] = {"zero", "one", "two"};

static int bump(int k);
static int walk(int n);
static int name_len(int i);

__attribute__((noinline)) static int driver(int k)
{
  int a = bump(k);
  int b = walk(7);
  int c = name_len(k % 3);
  return a * 1000 + b * 10 + c + counter;
}

__attribute__((noinline)) static int bump(int k)
{
  counter += k;
  return table[k & 7] + counter;
}

__attribute__((noinline)) static int walk(int n)
{
  if (n <= 0)
    return table[0];
  return table[n & 7] + walk(n - 1) + bump(1);
}

__attribute__((noinline)) static int name_len(int i)
{
  int n = 0;
  while (names[i][n])
    n++;
  return n + bump(0);
}

int main(void)
{
  printf("%d\n", driver(5));
  int before = counter;
  int second = driver(2);
  printf("%d %d\n", before, second);
  return 0;
}
