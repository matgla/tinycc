/* Branching on an inlined predicate (source/opt/flat/cfg/bool_diamond_branch.c).
 *
 * `if (a && b)` written in the `if` compiles to two branches, but the same
 * test behind a `static inline` has to MATERIALIZE a 0/1, so inlining leaves a
 * diamond whose merge is immediately re-tested: cmp/bne/orrs/ite/movne/moveq/
 * b/movs/cbz where cmp/bne/orrs/beq is enough.  Both functions here must
 * compile to the SAME shape -- no ITE, one branch per term.
 *
 * This is __aeabi_dadd's prologue: four inlined is_nan_parts/is_zero_parts
 * predicates, all on the common path. */
typedef unsigned long long u64;

static inline int is_nan_parts(int exp, u64 mant) { return (exp == 0x7FF) && (mant != 0); }
static inline int either_zero(int a, int b) { return (a == 0) || (b == 0); }

int sink(int x) __attribute__((noinline));

int inlined_and(int e, u64 m, int x)
{
  if (is_nan_parts(e, m))
    return sink(x);
  return x + 1;
}

int written_and(int e, u64 m, int x)
{
  if (e == 0x7FF && m != 0)
    return sink(x);
  return x + 1;
}

int inlined_or(int a, int b, int x)
{
  if (either_zero(a, b))
    return sink(x);
  return x + 1;
}

int written_or(int a, int b, int x)
{
  if (a == 0 || b == 0)
    return sink(x);
  return x + 1;
}
