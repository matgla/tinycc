/* byte_store_merge must not move a store before a label that jumps into it.
 *
 * The four byte stores can be merged into one word store, but the store at L
 * is a control-flow entry point.  Entering at L must still execute the stores
 * that follow it.
 */
#include <stdio.h>

char g[4];

__attribute__((noinline)) void store_bytes(int x)
{
  if (x)
    goto L;
  g[0] = 1;
L:
  g[1] = 2;
  g[2] = 3;
  g[3] = 4;
}

int main(void)
{
  store_bytes(1);
  printf("%d %d %d %d\n", g[0], g[1], g[2], g[3]);
  return 0;
}
