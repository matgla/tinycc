/*
 * Bug: a wide `unsigned long long` bit-field wrongly truncates the result of
 * ops whose other operand is a plain 64-bit value, and of shifts where the
 * bit-field is only the shift count.
 *
 * C11 6.7.2.1p10: a bit-field has an integer type of its declared width, so
 * arithmetic between bit-fields wraps at that width (s.x + s.x -> 40 bits).
 * But the result type is the *usual arithmetic conversion* of the operands:
 *   - a plain `unsigned long long` operand widens the result to 64 bits
 *     (Y + s.x, Y << s.x, Y += s.x must NOT truncate);
 *   - a shift's type is that of its left operand only (s.x << 1 truncates,
 *     Y << s.x does not);
 *   - two bit-fields of different widths take the larger width (s.x + s.y).
 * Narrow (<= int) and signed bit-fields never truncate here.
 *
 * gen_op_impl (source/frontend/gen/op/op.c) used to mask whenever *either*
 * operand was a wide unsigned bit-field, ignoring the other operand's width
 * and the shift's left-only typing.  Expected values are gcc's.
 */
#include <stdio.h>
#include <stdint.h>

struct S {
  unsigned long long x : 40;
  unsigned long long y : 50;
  unsigned int         n : 10;
} s;

volatile unsigned long long Y;

static int fails = 0;

static void chk(const char *name, unsigned long long got, unsigned long long want)
{
  if (got != want) {
    printf("FAIL %s: got %llx want %llx\n", name, got, want);
    fails++;
  } else {
    printf("OK %s = %llx\n", name, got);
  }
}

int main(void)
{
  /* --- Reproducer: a plain 64-bit operand must win (no truncation). --- */
  s.x = 1;
  Y = 1ULL << 50;
  {
    unsigned long long a = Y + s.x;
    unsigned long long b = Y << s.x;
    unsigned long long c = Y;
    c += s.x;
    chk("a", a, 0x4000000000001ULL);
    chk("b", b, 0x8000000000000ULL);
    chk("c", c, 0x4000000000001ULL);
  }

  /* --- The bit-field governs: wrap at its own width. --- */
  s.x = (1ULL << 40) - 1;
  {
    chk("xx",   s.x + s.x, 0xfffffffffeULL);
    chk("xmul", s.x * s.x, 1ULL);
    chk("xshl", s.x << 1,  0xfffffffffeULL);
    chk("xone", s.x + 1,   0ULL);
  }

  /* --- Mixed widths: the larger bit-field width governs. --- */
  s.y = (1ULL << 50) - 1;
  {
    chk("xy", s.x + s.y, 0xfffffffffeULL);
  }

  /* --- Narrow (<= int) bit-field: never truncates. --- */
  s.n = (1u << 10) - 1;
  {
    chk("nn", s.n + s.n, 0x7feULL);
  }

  printf("%s\n", fails ? "FAILED" : "ALL PASSED");
  return fails;
}
