/* Guard: entry_store_prop must not forward an initializer past a runtime-indexed
 * store made through a pointer that lives in a VAR.
 *
 * `int buf[N] = {0};` is a run of constant entry-block stores, and
 * entry_store_prop forwards those into later reads of the same slots.  A write
 * through `p[i]` with a runtime `i` has to invalidate every entry at or above
 * the array's base, which it does by resolving the store's base pointer back to
 * a frame offset.
 *
 * The hole this pins: an inlined pointer parameter lands as
 * `V <-- Addr[StackLoc[buf]] [STORE]`, a STORE rather than an ASSIGN/LEA, and
 * the base map only recorded the ASSIGN/LEA forms.  With no base for V the
 * `V + runtime` address was unresolvable, nothing got invalidated, and the reads
 * below folded back to the initializer's zeros — the copy loop ran and its
 * results were thrown away.  Both the STORE form and using a VAR as the base of
 * the indexing ADD have to be modelled for this to stay correct.
 *
 * Trip counts come from a volatile so the loops stay runtime-indexed after
 * inlining; with literal bounds the bodies fold away at -O2 before this shape
 * ever reaches the pass, and the case silently stops covering anything.  All
 * reads are of a slot other than element 0 — element 0 is separately
 * invalidated by the plain address-taken rule, so checking only it would pass
 * with the bug present.
 */
#include <stdio.h>

volatile int vn2 = 2;
volatile int vn4 = 4;
volatile int vn5 = 5;

void fill(int *dst, const int *src, int n)
{
  for (int i = 0; i < n; i++)
    dst[i] = src[i];
}

/* The reported shape: inlined pointer parameter, runtime-indexed store. */
int inlined_param_base(int n)
{
  int src[5] = {1, 2, 3, 4, 5};
  int dst[5] = {0};
  fill(dst, src, n);
  return dst[1] + dst[2] + dst[3] + dst[4];
}

/* Same, but the pointer is an explicit local rather than a parameter. */
int local_ptr_base(int n)
{
  int buf[4] = {0};
  int *p = buf;
  for (int i = 0; i < n; i++)
    p[i] = i + 7;
  return buf[3];
}

/* Pointer VAR carrying a constant displacement off the base. */
int local_ptr_offset_base(int n)
{
  int buf[6] = {0};
  int *p = buf + 2;
  for (int i = 0; i < n; i++)
    p[i] = i + 100;
  return buf[2] + buf[5];
}

/* A slot the loop never writes: the invalidation is conservative over the whole
 * array, so this must still read back the initializer. */
int untouched_slot(int n)
{
  int buf[4] = {0, 0, 0, 9};
  int *p = buf;
  for (int i = 0; i < n; i++)
    p[i] = 1;
  return buf[3];
}

/* Collected before any check runs: interleaving the comparisons with printf
 * perturbs the inlining enough that some cases stop reproducing. */
int res[4];

int main(void)
{
  int fails = 0;

  res[0] = inlined_param_base(vn5);
  res[1] = local_ptr_base(vn4);
  res[2] = local_ptr_offset_base(vn4);
  res[3] = untouched_slot(vn2);

  if (res[0] != 14) { printf("inlined_param_base: %d\n", res[0]); fails++; }
  if (res[1] != 10) { printf("local_ptr_base: %d\n", res[1]); fails++; }
  if (res[2] != 203) { printf("local_ptr_offset_base: %d\n", res[2]); fails++; }
  if (res[3] != 9) { printf("untouched_slot: %d\n", res[3]); fails++; }

  printf("fails=%d\n", fails);
  return 0;
}
