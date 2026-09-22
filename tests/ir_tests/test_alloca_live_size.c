/* alloca's size can stay live after the allocation: the new stack top was
 * computed in the size's own register, so every later use of n read the new
 * SP instead -- memset got a stack address as its length and faulted.  A VLA
 * whose length variable is read afterwards is the same shape. */
#include <stdio.h>
#include <string.h>

static volatile int sunk;

__attribute__((noinline)) static void sink(int x)
{
  sunk += x;
}

__attribute__((noinline)) static int with_alloca(int n)
{
  char *p = __builtin_alloca(n);
  memset(p, 1, n);
  sink(p[n - 1]);
  return p[0] + n;
}

__attribute__((noinline)) static int with_vla(int n)
{
  char buf[n];
  memset(buf, 2, n);
  sink(buf[n - 1]);
  return buf[0] * 100 + n;
}

int main(void)
{
  printf("%d %d\n", with_alloca(40), with_vla(24));
  return 0;
}
