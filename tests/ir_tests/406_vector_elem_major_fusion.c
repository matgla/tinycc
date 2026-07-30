/* Guard: element-major fusion of chained GCC vector_size expressions.
 *
 * gen_op_vector records a "recipe" for the temp slot each whole-vector op
 * produces, and a consuming op recomputes elements inline instead of reloading
 * them, deleting the producer's element loop.  The fused chain must compute
 * exactly what the phase-major lowering did, so every shape below is checked
 * against a scalar reference computed with the same operand values.
 *
 * Coverage aimed at the ways a recipe can go wrong:
 *  - two-deep and three-deep chains (recursive recomputation)
 *  - both operands being recipes (f6-style diamond)
 *  - comparison results (-1/0 masks) feeding a further op
 *  - a compound literal materialised between a recipe and its use, which must
 *    not invalidate the recipe (its stores hit only its own fresh storage)
 *  - a *call* between a recipe and its use, which MUST invalidate it because
 *    the callee can write through the pointer the recipe would reload
 *  - reuse of a named vector variable, which is never a recipe
 *  - unsigned elements and shifts, where signedness picks the compare/shift
 */
#include <stdio.h>

#define N 8
typedef int V __attribute__((vector_size(N * sizeof(int))));
typedef unsigned int W __attribute__((vector_size(N * sizeof(unsigned int))));

static int fails;

static void check(const char *name, const V *got, const int *want)
{
  int i;
  for (i = 0; i < N; i++)
  {
    int g = ((const int *)got)[i];
    if (g != want[i])
    {
      printf("FAIL %s[%d]: got %d want %d\n", name, i, g, want[i]);
      fails++;
    }
  }
}

static void checku(const char *name, const W *got, const unsigned *want)
{
  int i;
  for (i = 0; i < N; i++)
  {
    unsigned g = ((const unsigned *)got)[i];
    if (g != want[i])
    {
      printf("FAIL %s[%d]: got %u want %u\n", name, i, g, want[i]);
      fails++;
    }
  }
}

/* two-deep chain with constant literals on both ops */
__attribute__((noinline)) static void f_and_xor(V *p)
{
  *p = (*p & ((V){1, 1, 1, 1, 1, 1, 1, 1})) ^ ((V){3, 3, 3, 3, 3, 3, 3, 3});
}

/* three-deep chain */
__attribute__((noinline)) static void f_deep(V *p, V *q)
{
  *p = ((*p & *q) | ((V){8, 8, 8, 8, 8, 8, 8, 8})) - *q;
}

/* both operands of the outer op are recipes; the shared operand is reloaded */
__attribute__((noinline)) static void f_diamond(V *p, V *q, V *r)
{
  *p = (*p & *r) == (*q & *r);
}

/* a comparison mask feeding a further arithmetic op */
__attribute__((noinline)) static void f_mask_then_and(V *p, V *q)
{
  *p = (*p == *q) & ((V){7, 7, 7, 7, 7, 7, 7, 7});
}

/* xor-compare: the shape where GCC folds (a^b)==b to a==0 */
__attribute__((noinline)) static void f_xor_cmp(V *p, V *q)
{
  *p = (*p ^ *q) == *q;
}

/* unsigned elements + shift: signedness must survive the fusion */
__attribute__((noinline)) static void f_ushift(W *p, W *q)
{
  *p = *p < (((const W){1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u}) << *q);
}

/* a named vector local is not a recipe; the chain through it must still work */
__attribute__((noinline)) static void f_via_named(V *p, V *q)
{
  V t = *p & *q;
  *p = t ^ *q;
}

/* A call lands between a recipe and its consumer: the recipe must be dropped,
 * because the callee could write through any pointer it would reload.  The
 * comma sequences clobber() before gz is read; the callee touches neither p
 * nor q, so the expected value does not depend on the (unspecified) order in
 * which the two operands of `^` are evaluated. */
static V gz;
__attribute__((noinline)) static void clobber(void)
{
  int i;
  for (i = 0; i < N; i++)
    ((int *)&gz)[i] = 100 + i;
}
__attribute__((noinline)) static void f_call_between(V *p, V *q)
{
  *p = (*p & *q) ^ (clobber(), gz);
}

int main(void)
{
  V p, q, r;
  W up, uq;
  int want[N];
  unsigned uwant[N];
  int base_p[N], base_q[N], base_r[N];
  unsigned base_up[N], base_uq[N];
  int i;

  for (i = 0; i < N; i++)
  {
    base_p[i] = (i * 37) ^ 0x5a;
    base_q[i] = (i * 11) + 3;
    base_r[i] = (i & 1) ? -1 : 0x0f0f;
    base_up[i] = (unsigned)(i * 0x01010101u);
    base_uq[i] = (unsigned)(i % 5);
  }

#define RESET()                                                                                    \
  do                                                                                               \
  {                                                                                                \
    for (i = 0; i < N; i++)                                                                        \
    {                                                                                              \
      ((int *)&p)[i] = base_p[i];                                                                  \
      ((int *)&q)[i] = base_q[i];                                                                  \
      ((int *)&r)[i] = base_r[i];                                                                  \
    }                                                                                              \
  } while (0)

  RESET();
  for (i = 0; i < N; i++)
    want[i] = (base_p[i] & 1) ^ 3;
  f_and_xor(&p);
  check("and_xor", &p, want);

  RESET();
  for (i = 0; i < N; i++)
    want[i] = ((base_p[i] & base_q[i]) | 8) - base_q[i];
  f_deep(&p, &q);
  check("deep", &p, want);

  RESET();
  for (i = 0; i < N; i++)
    want[i] = ((base_p[i] & base_r[i]) == (base_q[i] & base_r[i])) ? -1 : 0;
  f_diamond(&p, &q, &r);
  check("diamond", &p, want);

  RESET();
  for (i = 0; i < N; i++)
    want[i] = ((base_p[i] == base_q[i]) ? -1 : 0) & 7;
  f_mask_then_and(&p, &q);
  check("mask_then_and", &p, want);

  RESET();
  for (i = 0; i < N; i++)
    want[i] = ((base_p[i] ^ base_q[i]) == base_q[i]) ? -1 : 0;
  f_xor_cmp(&p, &q);
  check("xor_cmp", &p, want);

  RESET();
  for (i = 0; i < N; i++)
    want[i] = (base_p[i] & base_q[i]) ^ base_q[i];
  f_via_named(&p, &q);
  check("via_named", &p, want);

  for (i = 0; i < N; i++)
  {
    ((unsigned *)&up)[i] = base_up[i];
    ((unsigned *)&uq)[i] = base_uq[i];
  }
  for (i = 0; i < N; i++)
    uwant[i] = (base_up[i] < (1u << base_uq[i])) ? 0xffffffffu : 0u;
  f_ushift(&up, &uq);
  checku("ushift", &up, uwant);

  RESET();
  for (i = 0; i < N; i++)
    want[i] = (base_p[i] & base_q[i]) ^ (100 + i);
  f_call_between(&p, &q);
  check("call_between", &p, want);

  printf("fails=%d\n", fails);
  return 0;
}
