/* A local captured by a nested function is read and written through the
 * static chain by any call that runs the child, but the parent's IR only
 * names its own references.  -O0 sized its slot by those alone, so locals the
 * parent never reads again shared ONE slot (q read as r), and a capture first
 * written by a child could share the slot of a local still live there. */
#include <stdio.h>

static int use(int v) { return v + 1; }

int readers(void)
{
  int p = 4, q = 5, r = 6;
  int first(int k) { int u = k * 2, w = u + p; return w + q; }
  int middle(int k) { int inner(int j) { return j + r + p; } int s = inner(k); return s * q; }
  int last(void) { return p * 100 + q * 10 + r; }
  printf("%d %d %d\n", first(1), middle(2), last());
  p = 7;
  r = 9;
  return first(1) + middle(2) + last();
}

int writer_first(int n)
{
  int x;
  void set(int v) { x = v; }
  int y = n * 3;
  set(n + 40);
  int z = use(y);
  return x * 1000 + z;
}

int main(void)
{
  printf("readers %d\n", readers());
  printf("writer %d %d\n", writer_first(1), writer_first(5));
  return 0;
}
