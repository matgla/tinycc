/* One level of a recursive function is expanded into itself at -O1/-O2
 * (source/frontend/gen/builtin/call.c, `self_inline_ok`).  That halves the
 * dynamic call count, which is the point -- but only if each expanded copy is
 * reached exactly when the call it replaced would have been.  A copy left
 * outside the guard that protected the call would run one extra time per
 * level, which no static instruction count can distinguish from the correct
 * shape: both hold two copies of the body and one call.
 *
 * So count the side effects at run time instead.  Every counter below is
 * bumped by the same statement that performs the guarded work, so its final
 * value IS how often that level executed.
 */
#include <stdio.h>

volatile int g;
volatile int sink;

static int walk_reads;
static void walk(int n)
{
  int t = g;
  walk_reads++;
  sink = t;
  if (n)
    walk(n - 1);
}

static int fib_bodies;
static int fib(int n)
{
  fib_bodies++;
  if (n <= 1)
    return n;
  return fib(n - 1) + fib(n - 2);
}

/* Singly recursive with work on the way back out: the order of the side
 * effects matters as much as the count. */
static int trace_len;
static char trace[32];
static void down(int n)
{
  trace[trace_len++] = (char)('a' + n);
  if (n)
    down(n - 1);
  trace[trace_len++] = (char)('A' + n);
}

int main(void)
{
  g = 7;

  walk(10);
  printf("walk_reads = %d\n", walk_reads);      /* 11 levels, one read each */

  int f = fib(12);
  printf("fib = %d bodies = %d\n", f, fib_bodies); /* 2*fib(13)-1 == 465 */

  down(3);
  trace[trace_len] = 0;
  printf("trace = %s\n", trace);

  if (walk_reads == 11 && f == 144 && fib_bodies == 465 && trace_len == 8)
    printf("PASS\n");
  else
    printf("FAIL\n");
  return 0;
}
