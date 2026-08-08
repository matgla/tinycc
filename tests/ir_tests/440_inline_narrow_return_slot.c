/* Inline expansion of a function with a sub-word return type left the upper
 * bytes of its return slot uninitialised.
 *
 * The slot is carved out at a minimum of 4 bytes (the rsize clamp next to
 * inline_ret_loc in tccgen.c), but the `return` stored the value at the
 * return type's own width while the consumer read the slot as a word:
 *
 *     strb.w  r1, [sp, #4]     <- 1 byte written
 *     ldr     r0, [sp, #4]     <- 4 bytes read
 *     cmp     r0, #0
 *
 * so `if (u8_returning_fn(...))` tested three bytes of stack garbage and went
 * whichever way the frame happened to be dirty.  It surfaced as a spurious
 * `immediate substituted into barrel-shift-annotated src2` abort once
 * tcc_ir_barrel_shift_at (uint8_t) started being inlined into
 * tcc_ir_set_src2, but nothing about it is specific to those two.
 *
 * Fix: store the whole word into the slot -- the value is already zero/sign
 * extended in its register, so that is correct for a narrow read of the slot
 * as well as for the word read the consumer performs.
 *
 * The dirty-frame helper matters: with a clean zero frame the bug is
 * invisible, so the test fills the stack region the callee will reuse first.
 */
#include <stdio.h>

typedef struct
{
  unsigned char *tab;
  int len;
} Side;

__attribute__((noinline)) static void dirty_frame(void)
{
  volatile unsigned int pad[24];
  unsigned int i;
  for (i = 0; i < 24; i++)
    pad[i] = 0xdeadbe00u | i;
}

__attribute__((always_inline)) static inline unsigned char at(const Side *s, int i)
{
  if (!s->tab || i < 0 || i >= s->len)
    return 0;
  return s->tab[i];
}

__attribute__((always_inline)) static inline short at_signed(const Side *s, int i)
{
  if (!s->tab || i < 0 || i >= s->len)
    return 0;
  return (short)-(int)s->tab[i];
}

__attribute__((noinline)) static int use_u8(const Side *s, int i, int cond)
{
  if (at(s, i) && cond)
    return 7;
  return 0;
}

__attribute__((noinline)) static int use_s16(const Side *s, int i)
{
  return at_signed(s, i);
}

int main(void)
{
  unsigned char data[4] = {0, 5, 0, 9};
  Side s;
  int fails = 0;

  s.tab = data;
  s.len = 4;

  /* Index 0 holds 0, so `at()` returns 0 and the && must be false.  This is
   * the case the bug broke: the zero came back as 0x??????00. */
  dirty_frame();
  if (use_u8(&s, 0, 1) != 0)
    fails++, printf("FAIL u8 zero-value: %d\n", use_u8(&s, 0, 1));

  /* Out of range -> the early `return 0` path, same exposure. */
  dirty_frame();
  if (use_u8(&s, 99, 1) != 0)
    fails++, printf("FAIL u8 out-of-range\n");

  /* Non-zero value still has to read back as true. */
  dirty_frame();
  if (use_u8(&s, 1, 1) != 7)
    fails++, printf("FAIL u8 nonzero\n");

  /* A narrow *signed* return has to keep its sign through the slot. */
  dirty_frame();
  if (use_s16(&s, 3) != -9)
    fails++, printf("FAIL s16 sign: %d\n", use_s16(&s, 3));

  dirty_frame();
  if (use_s16(&s, 0) != 0)
    fails++, printf("FAIL s16 zero: %d\n", use_s16(&s, 0));

  printf("%s\n", fails ? "FAILED" : "OK");
  return fails != 0;
}
