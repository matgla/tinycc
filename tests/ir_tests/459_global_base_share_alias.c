/* global_base_share re-bases a store, and the dead-store pass has to notice.
 *
 * global_base_share (source/opt/flat/memory/global_base_share.c) clusters
 * consecutive stores to neighbouring same-section globals onto one base
 * register: `a = 255; b = 1;` becomes a LEA of &a and two STORE_INDEXEDs at
 * offsets 0 and 4.  Every analysis downstream that identifies a global access
 * by its Sym* is then looking at the wrong name -- the store to `b` resolves
 * as (sym=a, off=4) while an ordinary read of `b` resolves as (sym=b, off=0).
 *
 * dce_dead_global_stores compared those two and concluded they could not be
 * the same location, so a store whose value was still being read looked dead
 * and went away: `a = 255; b = 1; <something>; b -= 1;` left `b` reading its
 * .bss zero and printed -1.  It now compares LINKER ADDRESSES -- section index
 * plus st_value folded into the offset -- so a re-based store still matches
 * the reads it feeds.
 *
 * `killed_*` are that shape.  The rest are the cases the address comparison
 * must still get right: a read between two stores keeps the first alive, a
 * genuinely dead store is still removed, two different sections never alias,
 * and an unknown index off one global may reach any of its neighbours.
 *
 * Everything is sequenced through named locals -- argument evaluation order is
 * unspecified, and these functions all read and write the same globals. */

#include <stdio.h>

int ga, gb, gc, gd;
static int sa, sb;
const int ra = 4, rb = 5;

/* The bug: the store to gb is re-based onto &ga, then dropped over the read. */
int killed_simple(void)
{
  ga = 255;
  gb = 1;
  gb -= 1;
  return ga * 1000 + gb;
}

/* Same, with an unrelated store through a pointer in between (what
 * __builtin_add_overflow's out-parameter looked like). */
int killed_via_ptr(void)
{
  unsigned int t = 0;
  ga = 7;
  gb = 9;
  __builtin_add_overflow(1, 1, &t);
  gb -= 1;
  return ga * 1000 + gb * 10 + (int)t;
}

/* A read between the two stores keeps the first one alive. */
int read_between(void)
{
  ga = 21;
  gb = 22;
  int t = gb;
  ga = 31;
  gb = 32;
  return t * 100 + gb;
}

/* Nothing reads the first pair, so those stores really are dead. */
int truly_dead(void)
{
  ga = 41;
  gb = 42;
  ga = 51;
  gb = 52;
  return ga * 100 + gb;
}

/* Statics land in their own storage; the cluster must not reach across. */
int statics_too(void)
{
  sa = 61;
  sb = 62;
  int t = sa;
  sa = 71;
  sb = 72;
  return t * 10000 + sa * 100 + sb;
}

/* A different section entirely -- .rodata cannot alias .bss. */
int other_section(void)
{
  ga = 81;
  gb = 82;
  return ra * 10000 + rb * 100 + ga + gb;
}

/* An index the compiler cannot resolve reaches an unknown element of its own
 * object, so it invalidates every pending store to that object.  (Indexing off
 * a scalar to reach its neighbour would be undefined, so this uses an array.) */
int arr[4];

int unknown_index(int i)
{
  arr[0] = 91;
  arr[1] = 92;
  arr[2] = 93;
  arr[i] = 7;
  return arr[0] * 10000 + arr[1] * 100 + arr[2];
}

/* Four in a row, with the last read back through its own name. */
int long_cluster(void)
{
  ga = 1;
  gb = 2;
  gc = 3;
  gd = 4;
  gd -= 1;
  gc -= 1;
  return ga * 1000 + gb * 100 + gc * 10 + gd;
}

int main(void)
{
  int r1 = killed_simple();
  int r2 = killed_via_ptr();
  int r3 = read_between();
  int r4 = truly_dead();
  int r5 = statics_too();
  int r6 = other_section();
  int r7 = unknown_index(1);
  int r8 = unknown_index(2);
  int r9 = long_cluster();
  printf("%d %d %d %d\n", r1, r2, r3, r4);
  printf("%d %d %d %d %d\n", r5, r6, r7, r8, r9);
  printf("OK\n");
  return 0;
}
