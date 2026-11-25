/* Chapter 3 -- catching undefined behaviour at run time (README.md).
 *
 * Each step below does something C leaves undefined.  Without a sanitizer
 * the program prints a wrong answer and carries on.  With one:
 *
 *   tcc -ccompat -fsanitize=undefined 03_sanitizers.c -o ub
 *       trap mode (the default): the first bad step stops the program
 *       with `udf #254` (a fault on the board, a UDF stop under ptrace);
 *       no runtime library is needed
 *   tcc -ccompat -fsanitize=undefined -fsanitize-recover=undefined \
 *       03_sanitizers.c -o ub
 *       handler mode: libc's __ubsan_handle_* say what and where on
 *       stderr, and the program goes on (03_sanitizers.expect)
 *
 * The inputs come from a volatile so that nothing is folded at compile
 * time: a constant overflow is a compile-time warning instead (chapter 1). */
#include <stdio.h>

static volatile int input[] = {2147483647, 0, 33, 4};

static int add(int a, int b)
{
  return a + b; /* signed overflow */
}

static int divide(int a, int b)
{
  return a / b; /* division by zero */
}

static unsigned shift(unsigned v, int n)
{
  return v << n; /* a count >= 32 */
}

static struct
{
  int table[4];
  int after;
} data = {{10, 20, 30, 40}, 99};

static int element(int i)
{
  return data.table[i]; /* past the end */
}

static int *volatile nowhere;

int main(void)
{
  int big = input[0], zero = input[1], wide = input[2], four = input[3];
  int *p = nowhere;

  printf("step 1\n");
  printf("load: %s\n", *p == 0x12345678 ? "?" : "read"); /* through null */
  printf("step 2\n");
  printf("add: %d\n", add(big, 1));
  printf("step 3\n");
  printf("divide: %d\n", divide(big, zero));
  printf("step 4\n");
  printf("shift: %u\n", shift(1, wide));
  printf("step 5\n");
  printf("element: %d\n", element(four));
  printf("done\n");
  return 0;
}
