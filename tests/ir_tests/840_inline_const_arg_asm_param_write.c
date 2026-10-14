/* always_inline + a constant argument: the inliner records the parameter's
 * slot -> constant in tcc_state->inline_const_args, and parse_asm_operands
 * substitutes that constant into EVERY operand naming the slot, ignoring the
 * operand's role and whether the body wrote the parameter first.
 * Two symptoms, one root cause:
 *   - f(): the body does x = x + 1 before the asm reads x, but the asm gets
 *     the ORIGINAL argument (5) instead of the current value (6) at -O1/-O2/-Os.
 *   - h(): the "+r"(x) OUTPUT operand is replaced by the constant, so test_lvalue()
 *     rejects it -> spurious "lvalue expected" compile error at -O1/-O2/-Os.
 * Correct at -O0. Returns 0 only when both are right. */
static inline __attribute__((always_inline)) int f(int x)
{
  int r;
  x = x + 1;
  __asm__ volatile("mov %0, %1" : "=r"(r) : "r"(x));
  return r;
}

static inline __attribute__((always_inline)) int h(int x)
{
  __asm__ volatile("add %0, %0, #1" : "+r"(x));
  return x;
}

int main(void)
{
  return (f(5) != 6) | (h(5) != 6);
}
