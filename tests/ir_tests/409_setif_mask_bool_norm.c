/* Guard: the SETIF {LT,EQ,GT} state-mask algebra (ssa:setif_mask_fold) and the
 * 0/1 re-normalization elimination (ssa:bool_norm) — gcc PR107881.
 *
 * Two booleans built from compares of the SAME operand pair live in a closed
 * three-state algebra, so `(a<b) ^ (a>b)` collapses to `a != b` and
 * `(a<b) ^ (a>=b)` to constant 1.  Every combination of {lt,le,gt,ge,eq,ne}
 * under {^, ==, !=} is checked here against the unfolded value, in both a
 * plain and a `volatile _Bool` variant — the volatile one must keep every
 * mandated access while still dropping the redundant `!= 0` normalization.
 *
 * The traps this pins down:
 *   - unsigned compares must not merge with signed ones over the same pair
 *     (the mask maps back to a DIFFERENT condition for each signedness);
 *   - a boolean whose value is NOT provably 0/1 (a plain int, a char load,
 *     a value read through a pointer) must not take the `x != y` -> `x ^ y`
 *     shortcut;
 *   - an address-taken local can be written behind the compiler's back, so
 *     its loads carry no {0,1} range;
 *   - rewriting a SETIF (dest+src1) into a binary op needs a THIRD operand
 *     slot, which the pool packs densely: reusing the old two-slot block
 *     silently overwrites the following instruction's dest.
 */
#include <stdio.h>

static int fails;

static void expect(const char *what, int got, int want)
{
  if (got != want)
  {
    printf("FAIL %s: got %d want %d\n", what, got, want);
    fails++;
  }
}

#define PLAIN(name, o1, o2, o3)                                                                    \
  __attribute__((noinline)) static _Bool p_##name(int a, int b)                                    \
  {                                                                                                \
    _Bool x = o1(a, b);                                                                            \
    _Bool y = o2(a, b);                                                                            \
    return o3(x, y);                                                                               \
  }
#define VOL(name, o1, o2, o3)                                                                      \
  __attribute__((noinline)) static _Bool v_##name(int a, int b)                                    \
  {                                                                                                \
    volatile _Bool x = o1(a, b);                                                                   \
    volatile _Bool y = o2(a, b);                                                                   \
    return o3(x, y);                                                                               \
  }

#define o_lt(a, b) ((a) < (b))
#define o_le(a, b) ((a) <= (b))
#define o_gt(a, b) ((a) > (b))
#define o_ge(a, b) ((a) >= (b))
#define o_eq(a, b) ((a) == (b))
#define o_ne(a, b) ((a) != (b))
#define o_xor(a, b) ((a) ^ (b))

/* The interesting corners of the algebra: complementary pairs (mask 0b111 ->
 * constant 1), disjoint pairs (mask 0b000 -> constant 0), and pairs whose
 * combination is a plain relational op. */
#define CASES(M)                                                                                   \
  M(lt_ge_xor, o_lt, o_ge, o_xor)   /* -> 1        */                                              \
  M(lt_ge_ne, o_lt, o_ge, o_ne)     /* -> 1        */                                              \
  M(lt_ge_eq, o_lt, o_ge, o_eq)     /* -> 0        */                                              \
  M(gt_le_eq, o_gt, o_le, o_eq)     /* -> 0        */                                              \
  M(lt_gt_xor, o_lt, o_gt, o_xor)   /* -> a != b   */                                              \
  M(lt_gt_eq, o_lt, o_gt, o_eq)     /* -> a == b   */                                              \
  M(le_ge_eq, o_le, o_ge, o_eq)     /* -> a == b   */                                              \
  M(le_ge_ne, o_le, o_ge, o_ne)     /* -> a != b   */                                              \
  M(lt_eq_xor, o_lt, o_eq, o_xor)   /* -> a <= b   */                                              \
  M(gt_eq_xor, o_gt, o_eq, o_xor)   /* -> a >= b   */                                              \
  M(lt_ne_ne, o_lt, o_ne, o_ne)     /* -> a > b    */                                              \
  M(gt_ne_ne, o_gt, o_ne, o_ne)     /* -> a < b    */                                              \
  M(le_lt_xor, o_le, o_lt, o_xor)   /* -> a == b   */                                              \
  M(ge_gt_xor, o_ge, o_gt, o_xor)   /* -> a == b   */                                              \
  M(eq_ne_xor, o_eq, o_ne, o_xor)   /* -> 1        */                                              \
  M(lt_lt_eq, o_lt, o_lt, o_eq)     /* -> 1        */                                              \
  M(lt_lt_xor, o_lt, o_lt, o_xor)   /* -> 0        */

CASES(PLAIN)
CASES(VOL)

#define REF(name, o1, o2, o3)                                                                      \
  {                                                                                                \
    _Bool rx = o1(a, b), ry = o2(a, b);                                                            \
    int want = o3(rx, ry);                                                                         \
    expect("p_" #name, p_##name(a, b), want);                                                      \
    expect("v_" #name, v_##name(a, b), want);                                                      \
  }

/* Signed and unsigned compares of the same pair are DIFFERENT predicates; a
 * mask merge across them would make this return the wrong answer whenever the
 * operands straddle the signed/unsigned boundary. */
__attribute__((noinline)) static _Bool mixed_sign(int a, int b)
{
  _Bool x = a < b;
  _Bool y = (unsigned)a < (unsigned)b;
  return x ^ y;
}

/* Not booleans: `x != y` here is NOT `x ^ y`. */
__attribute__((noinline)) static _Bool int_ne(int x, int y) { return x != y; }
__attribute__((noinline)) static _Bool char_ne(unsigned char *p, unsigned char *q)
{
  return *p != *q;
}
__attribute__((noinline)) static _Bool deref_ne(int *p, int *q) { return *p != *q; }

/* Address-taken local: the callee writes a non-boolean through the pointer, so
 * the load afterwards has no {0,1} range and `t != 0` must stay a real test. */
__attribute__((noinline)) static void poke(unsigned char *p) { *p = 7; }
__attribute__((noinline)) static _Bool addr_taken_local(int a, int b)
{
  unsigned char t = (a < b);
  poke(&t);
  return t != 0;
}

/* A boolean feeding a compare whose result feeds ANOTHER compare: the rewrite
 * of the inner SETIF must not disturb the outer instruction's operands (the
 * dense operand pool makes an in-place widening corrupt the next quad). */
__attribute__((noinline)) static int chained(int a, int b, int c, int d)
{
  _Bool x = a < b;
  _Bool y = c < d;
  _Bool z = x != y;
  return z ? 100 : 200;
}

/* `x & 1` is boolean for any x. */
__attribute__((noinline)) static _Bool masked_ne(int x, int y)
{
  return (x & 1) != (y & 1);
}

int main(void)
{
  static const int vals[] = {-10, -3, -1, 0, 1, 3, 10};
  int i, j;

  for (i = 0; i < (int)(sizeof(vals) / sizeof(vals[0])); i++)
    for (j = 0; j < (int)(sizeof(vals) / sizeof(vals[0])); j++)
    {
      int a = vals[i], b = vals[j];
      CASES(REF)
      expect("mixed_sign", mixed_sign(a, b), (a < b) ^ ((unsigned)a < (unsigned)b));
      expect("int_ne", int_ne(a, b), a != b);
      expect("deref_ne", deref_ne(&a, &b), a != b);
      expect("masked_ne", masked_ne(a, b), (a & 1) != (b & 1));
      expect("chained", chained(a, b, b, a), ((a < b) != (b < a)) ? 100 : 200);
    }

  {
    /* 2 != 3 is true, but 2 ^ 3 is 1 as well — use values whose XOR is a
     * different truth value from their inequality to catch a bad rewrite. */
    unsigned char p = 2, q = 3;
    expect("char_ne_23", char_ne(&p, &q), 1);
    p = 3;
    q = 3;
    expect("char_ne_33", char_ne(&p, &q), 0);
    p = 1;
    q = 3;
    expect("char_ne_13", char_ne(&p, &q), 1);
  }

  expect("addr_taken_local", addr_taken_local(0, 1), 1);
  expect("addr_taken_local2", addr_taken_local(1, 0), 1);

  printf("fails=%d\n", fails);
  return 0;
}
