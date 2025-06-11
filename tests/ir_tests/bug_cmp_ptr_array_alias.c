/* Regression: CMP identity-folding ignored operand lval-ness.  A struct with
 * `int stack[64]; int *ptr;` lays the pointer field immediately after the
 * array, so `&s->stack[64]` (the limit address) equals `&s->ptr`.  The bounds
 * check `s->ptr >= s->stack + N` lowers to `*(s+off) >=U (s+off)` — i.e.
 * `load(addr) >=U addr`.  The const_prop / cmp_expr_fold identity folder proved
 * the two operands' *address* expressions equal (both `base + off`) and folded
 * `x >= x` to always-true, dropping the push + the bound check entirely.
 *
 * This is exactly tcc's own `ifdef_stack` overflow guard (tccpp.c do_if):
 * mis-folding it made the FIRST `#if` in the predefs report "memory full
 * (ifdef)", so the self-hosted compiler couldn't preprocess anything.
 *
 * Fix: identity folding requires cmp_src1.is_lval == cmp_src2.is_lval; `*(p)`
 * and `p` are different values even when p's defining expression is identical.
 */

struct S {
  int stack[64];
  int *ptr;
};

static struct S s;

/* Push c, growing s.ptr; reports overflow exactly like tcc's do_if guard.
 * Must NOT be inlined-and-folded into an unconditional overflow. */
static int push(int c)
{
  if (s.ptr >= s.stack + 64)
    return -1;            /* overflow — must be reachable only when full */
  *s.ptr++ = c;
  return 0;
}

int main(void)
{
  s.ptr = s.stack;        /* start empty (done in a separate "function" so the
                             optimizer can't see ptr==&stack[0] inside push) */
  int ok = 0;
  for (int i = 0; i < 5; i++)
    ok += (push(i) == 0); /* all 5 must succeed: 0..4 < 64 */
  __builtin_printf("pushed=%d\n", ok);                 /* 5 */
  __builtin_printf("count=%ld\n", (long)(s.ptr - s.stack)); /* 5 */
  __builtin_printf("first=%d last=%d\n", s.stack[0], s.stack[4]); /* 0 4 */
  return 0;
}
