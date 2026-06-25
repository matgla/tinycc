/* Regression guard for struct stack-slot reuse / coalescing.
 *
 * Exercises the exact patterns under which a naive sret-buffer reuse
 * miscompiles (and which pr92904 does NOT cover): nested struct-returning
 * calls in one expression, a struct result feeding another call's arg, ?:
 * with struct operands, and interleaved different-sized buffers.  Any pass
 * that coalesces stack slots with incorrect liveness will corrupt one of
 * these and change `acc`. */
#include <stdio.h>

struct S { long long a, b; };
struct B { long long a, b, c, d; };

__attribute__((noinline)) struct S mk(long long x) { struct S s; s.a = x; s.b = x * 2 + 1; return s; }
__attribute__((noinline)) struct S addS(struct S p, struct S q) { struct S r; r.a = p.a + q.a; r.b = p.b + q.b; return r; }
__attribute__((noinline)) struct B mkB(long long x) { struct B b; b.a = x; b.b = x + 1; b.c = x + 2; b.d = x + 3; return b; }

volatile long long seed = 10;

int main(void)
{
  long long acc = 0;
  struct S a = mk(seed), b = mk(seed + 5);          /* sequential reuse */
  acc += a.a + a.b + b.a + b.b;                      /* 77 */
  struct S c = addS(mk(seed), mk(seed + 1));         /* nested sret buffers must coexist */
  acc += c.a + c.b;                                  /* 65 */
  struct S d = addS(mk(seed + 2), c);                /* struct result feeds an arg */
  acc += d.a + d.b;                                  /* 102 */
  struct S e = (seed > 0) ? mk(seed + 3) : mk(seed + 4); /* ?: struct operands */
  acc += e.a + e.b;                                  /* 40 */
  struct B f = mkB(seed);                            /* different-size buffer interleaved */
  acc += f.a + f.b + f.c + f.d;                      /* 46 */
  struct S g = mk(seed + 7);                         /* reuse after the big buffer */
  acc += g.a + g.b;                                  /* 52 */
  printf("acc=%lld\n", acc);                         /* 382 */
  return (int)(acc - 382); /* 0 on success */
}
