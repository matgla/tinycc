/* Regression test for barrel-shift fusion preserving memory operands.
 *
 * The fusion pass must not replace a dereferenced consumer operand with the
 * shift source, and it must not move a dereferenced shift source past an
 * intervening store.  Both cases are single-use shifts that otherwise match
 * the ARM barrel-shifter pattern.
 */
#include <stdio.h>

static int aligned_value __attribute__((aligned(16))) = 48;

__attribute__((noinline)) static int compare_loaded(int x, unsigned address_hi)
{
  return x == *(int *)(address_hi << 4);
}

__attribute__((noinline)) static int add_after_store(int x, int *p, int *q)
{
  int shifted = *p << 5;
  *q = 1;
  return x + shifted;
}

int main(void)
{
  int value = 3;
  int ok = 1;

  if (!compare_loaded(48, (unsigned)&aligned_value >> 4)) {
    printf("consumer-deref FAIL\n");
    ok = 0;
  }
  if (add_after_store(1, &value, &value) != 97) {
    printf("source-deref FAIL\n");
    ok = 0;
  }
  if (ok)
    printf("PASS\n");
  return ok ? 0 : 1;
}
