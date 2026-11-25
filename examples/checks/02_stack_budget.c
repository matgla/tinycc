/* Chapter 2 -- a stack budget (README.md).
 *
 * A YasOS process has a 4 to 8 KB stack.  run_unbounded() below has three
 * ways to blow it: a big local buffer, a variable length array, and
 * recursion.  run_bounded() does the same work in a known amount of stack.
 *
 *   tcc -ccompat -O2 -Wframe-larger-than=1024 -Wstack-usage=1024 -Wvla \
 *       -fstack-usage -fcallgraph-info=su -c 02_stack_budget.c
 *   python3 scripts/stack_usage.py 02_stack_budget.ci --unknown-budget 512
 *
 * 02_stack_budget.diag is what tcc says, 02_stack_budget.stack what the
 * script says.  The .su and .ci files land beside the object.  noinline
 * keeps each function's frame apart: at -O1 and up an inlined callee's
 * frame becomes part of its caller's. */
#include <stdio.h>
#include <string.h>

#define NOINLINE __attribute__((noinline))

/* 2 KB on the stack: over a 1 KB frame budget */
NOINLINE static int checksum_big(const char *name)
{
  char block[2048];
  int sum = 0;
  memset(block, 0, sizeof block);
  strncpy(block, name, sizeof block - 1);
  for (unsigned i = 0; i < sizeof block; i++)
    sum += (unsigned char)block[i];
  return sum;
}

/* the frame grows with n: nothing bounds it */
NOINLINE static int sum_squares(int n)
{
  int squares[n];
  int sum = 0;
  for (int i = 0; i < n; i++)
    squares[i] = i * i;
  for (int i = 0; i < n; i++)
    sum += squares[i];
  return sum;
}

/* recursion: as deep as the input */
NOINLINE static int depth(const char *s)
{
  if (*s != '(')
    return 0;
  return 1 + depth(s + 1);
}

/* the same work, bounded: a static buffer ... */
NOINLINE static int checksum_static(const char *name)
{
  static char block[2048];
  int sum = 0;
  memset(block, 0, sizeof block);
  strncpy(block, name, sizeof block - 1);
  for (unsigned i = 0; i < sizeof block; i++)
    sum += (unsigned char)block[i];
  return sum;
}

/* ... an array of a fixed size ... */
NOINLINE static int sum_squares_bounded(int n)
{
  int squares[16];
  int sum = 0;
  if (n > 16)
    n = 16;
  for (int i = 0; i < n; i++)
    squares[i] = i * i;
  for (int i = 0; i < n; i++)
    sum += squares[i];
  return sum;
}

/* ... and a loop */
NOINLINE static int depth_loop(const char *s)
{
  int d = 0;
  while (s[d] == '(')
    d++;
  return d;
}

NOINLINE void run_unbounded(void)
{
  printf("%d %d %d\n", checksum_big("ab"), sum_squares(4), depth("((("));
}

NOINLINE void run_bounded(void)
{
  printf("%d %d %d\n", checksum_static("ab"), sum_squares_bounded(4), depth_loop("((("));
}

int main(void)
{
  run_unbounded();
  run_bounded();
  return 0;
}
