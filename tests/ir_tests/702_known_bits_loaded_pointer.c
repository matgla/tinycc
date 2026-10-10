#include <stdio.h>

/* known_bits took `T <-- Taddr***DEREF***` (the pointer LOADED from a frame
 * slot) for the slot's address, so w.p[1] read the word after w.p in w. */
struct W { int *p; int k; };
static int g(struct W w) { return w.p[1]; }
static int g2(struct W w) { return w.p[0] + w.k; }

int main(void)
{
  int y[2] = {1, 2};
  struct W w = { y, 50 };
  printf("kb %d %d\n", g(w), g2(w));
  return 0;
}
