/* ssa:loop_bottom_test — the `while`-shaped loops ssa:loop_rotate cannot match.
 *
 * A loop whose body falls through from its header (the shape ssa:cfg_cleanup
 * leaves, and the one ssa:iv_strength_reduction produces once it eliminates a
 * counter against an end pointer) keeps its top test and an unconditional
 * back-edge.  ssa:loop_bottom_test copies the header CMP in front of the
 * back-edge and makes the back-edge the inverted conditional branch.
 *
 * Nothing here checks the SHAPE — these are correctness cases.  Each one is a
 * way the rewrite, or the copy forwarding that goes with it, can silently
 * produce a wrong value:
 *
 *   fill          — the canonical admitted walk (`str r,[p],#4` bottom-tested).
 *                   A dropped or duplicated final iteration shows up in the sum.
 *   fill_bytes    — byte stride, so the guard and the tail test disagree about
 *                   the last element if the inverted condition is wrong.
 *   swap3         — 20000801-1's shape: the walked pointer is written THREE
 *                   times in one body (`*bp++`, `*bp++`, `bp += 2`), so the
 *                   `T <- P` copies each name a DIFFERENT value.  Forwarding
 *                   them onto P stored through the already-incremented pointer
 *                   and byte-swapped the wrong pair.
 *   empty_trip    — zero-trip: the header guard must still be the thing that
 *                   decides, or the body runs once.
 *   one_trip      — exactly one iteration, the boundary the tail test owns.
 *   sum_offsets   — reads at fixed offsets off the walked pointer (the sha
 *                   W-expansion shape) alongside the bump.
 *   after_loop    — the walked pointer is read AFTER the loop, so the exit
 *                   value has to be the end pointer, not one step short.
 */
#include <stdio.h>

static int g[64];
static unsigned char b[32];

static void fill(int v)
{
  for (int i = 0; i < 64; i++)
    g[i] = v + i;
}

static int fill_bytes(unsigned char v)
{
  int s = 0;
  for (int i = 0; i < 32; i++)
    b[i] = (unsigned char)(v + i);
  for (int i = 0; i < 32; i++)
    s += b[i];
  return s;
}

/* gcc.c-torture 20000801-1's foo(): three writes of bp per iteration. */
static void swap3(char *bp, unsigned n)
{
  char c;
  char *ep = bp + n;
  char *sp;

  while (bp < ep)
  {
    sp = bp + 3;
    c = *sp;
    *sp = *bp;
    *bp++ = c;
    sp = bp + 1;
    c = *sp;
    *sp = *bp;
    *bp++ = c;
    bp += 2;
  }
}

static int empty_trip(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += g[i];
  return s;
}

static int sum_offsets(void)
{
  int s = 0;
  for (int i = 8; i < 64; i++)
    s += g[i - 8] + g[i - 3] + g[i];
  return s;
}

static int after_loop(void)
{
  int *p = g;
  int *end = g + 64;
  int s = 0;
  while (p < end)
    s += *p++;
  return s + (int)(p - g);
}

int main(void)
{
  int s = 0;
  unsigned int word = 0x11223344u;

  fill(1);
  for (int i = 0; i < 64; i++)
    s += g[i];
  printf("fill=%d bytes=%d\n", s, fill_bytes(2));

  swap3((char *)&word, sizeof(word));
  printf("swap3=%08x\n", word);
  swap3((char *)&word, sizeof(word));
  printf("swap3back=%08x\n", word);

  printf("empty=%d one=%d offsets=%d after=%d\n", empty_trip(0), empty_trip(1), sum_offsets(), after_loop());
  return 0;
}
