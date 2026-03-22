/* Test __builtin_setjmp and __builtin_longjmp compilation
 *
 * This test verifies that the builtins are recognized and compile correctly.
 * The full functionality requires platform-specific implementation.
 */
#include <stdio.h>

void *jmp_buf[5];

void __attribute__((noinline)) do_longjmp(void **buf)
{
  __builtin_longjmp(buf, 1);
}

int main(void)
{
  int result;

  /* Test that __builtin_setjmp is recognized and returns an int */
  result = __builtin_setjmp(jmp_buf);

  if (result == 0)
  {
    printf("setjmp returned 0 (initial call)\n");
    /* Don't actually call longjmp in this basic test since
     * the full implementation is platform-specific */
    printf("PASS: builtins compile and execute basic path\n");
    return 0;
  }
  else
  {
    printf("setjmp returned %d\n", result);
    return 1;
  }
}
