/* Regression: a function with stack-passed parameters that needs a frame
 * pointer (FP) miscompiled the load of a stack parameter into a transient.  The
 * transient was left "unresolved" by the allocator (PREG_NONE, frame offset 0),
 * and the backend lowered an offset-0 spill as `str rX, [FP, #0]`.  Under the
 * `push {r7,lr}; add r7,sp,#0` prologue, [FP,#0] is the SAVED frame record, so
 * the spill clobbered the caller's saved r7.  On return (`mov sp,r7; pop {r7}`)
 * the caller's frame pointer became garbage.
 *
 * Surfaced on YasOS as a HardFault (STKOF) in tinycc's own new_symtab(): its r7
 * frame pointer was overwritten with a stale stack value by malloc -> mmap()
 * (a 6-arg wrapper that builds a context struct from its stack params), so the
 * epilogue `mov sp, r7` faulted.  The same shape (>=5 args, struct built from
 * the stack params, address passed to an opaque callee) is reproduced here.
 *
 * Fix: reserve a scratch word below the locals for offset-0 ("unresolved")
 * spills and route fp_adjust_local_offset(0) there, so they never alias the
 * saved frame record.
 */
#include <stdio.h>

struct ctx { void *a; int b, c, d, e, f; void *r; };

/* Opaque so the call result/args stay materialized and the struct address must
 * be taken (forces an FP frame in `inner`). */
void sink(int n, const void *p);

int inner(void *a, int b, int c, int d, int e, int f)
{
  int result = 0;
  const struct ctx x = {.a = a, .b = b, .c = c, .d = d, .e = e, .f = f, .r = &result};
  sink(34, &x);
  return result;
}

/* `outer` keeps a value live across the call in a callee-saved register and
 * also uses an FP frame; if `inner` corrupts the caller's r7, `outer`'s own
 * locals/return are wrong (or it faults). */
int outer(int seed)
{
  int local[4] = {seed, seed + 1, seed + 2, seed + 3};
  int r = inner(local, 1, 2, 3, 4, 5);
  return r + local[0] + local[1] + local[2] + local[3];
}

void sink(int n, const void *p)
{
  const struct ctx *c = p;
  /* Write back through the result pointer so `inner` returns something. */
  *(int *)c->r = n + c->b + c->c + c->d + c->e + c->f;
}

int main(void)
{
  int r = outer(10);
  /* inner result = 34 + (1+2+3+4+5) = 49; outer adds local 10+11+12+13 = 46. */
  printf("r=%d\n", r);
  return (r == 95) ? 0 : 1;
}
