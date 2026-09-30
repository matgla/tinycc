#include <stdio.h>
#include <string.h>

/* A caller's object whose address escapes must stay live across an
 * always_inline expansion: the expansion used to end the lifetime of every
 * object of the function, and a later block-local was laid over it. */
struct table { long v[11]; };

__attribute__((noinline)) void fill(struct table *t, long x)
{
  for (int i = 0; i < 11; i++)
    t->v[i] = x + i;
}

__attribute__((noinline)) long sum(const struct table *t)
{
  long s = 0;
  for (int i = 0; i < 11; i++)
    s += t->v[i];
  return s;
}

__attribute__((noinline)) void scribble(char *b)
{
  memset(b, 0x55, 44);
}

static inline __attribute__((always_inline)) int odd(int a)
{
  return a & 1;
}

int main(void)
{
  struct table t;
  int n = odd(3);
  fill(&t, 100);
  n += odd(n + 4);
  {
    char buf[44];
    scribble(buf);
    n += buf[3] == 0x55;
  }
  {
    char other[44];
    scribble(other);
    n += other[40] == 0x55;
  }
  printf("sum=%ld n=%d\n", sum(&t), n);
  printf("first=%ld last=%ld\n", t.v[0], t.v[10]);
  return 0;
}
