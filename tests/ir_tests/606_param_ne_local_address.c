// toysh's expand_arg_nobrace() takes an optional output deck as its fifth
// (stack-passed) parameter and falls back to a local one: "if (!ant) ant =
// &deck;" ... "if (ant != &deck && ant->v) finish(ant);". With a caller's
// deck passed in, the comparison must be true.
#include <stdio.h>
#include <stdlib.h>

struct deck {
  long *v;
  int c;
};

__attribute__((noinline)) static void add(struct deck *d, long x)
{
  d->v = realloc(d->v, sizeof(long)*(d->c+1));
  d->v[d->c++] = x;
}

__attribute__((noinline)) static void finish(struct deck *d)
{
  d->c--;
}

__attribute__((noinline)) static int expand(struct deck *out, const char *s,
  unsigned flags, void **del, struct deck *ant, long *measure)
{
  struct deck deck = {0};
  int n = 0;

  if (!ant) ant = &deck;
  for (; *s; s++) {
    if (*s == '*' && !(flags&2)) add(ant, n);
    n++;
  }
  add(ant, 99);
  if (out) out->c = n;
  if (del) *del = 0;
  free(deck.v);
  if (ant != &deck && ant->v) finish(ant);
  if (measure) *measure = n;

  return n;
}

int main(void)
{
  struct deck d = {0}, o = {0};
  void *del = &o;
  long m = 0;
  int n = expand(&o, "a*b", 0, &del, &d, &m);

  printf("n=%d c=%d m=%ld o=%d\n", n, d.c, m, o.c);
  printf("local=%d\n", expand(0, "**", 0, 0, 0, 0));

  return 0;
}
