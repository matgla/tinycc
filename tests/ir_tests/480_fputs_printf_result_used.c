/* The frontend lowers fputs(s, f) to fwrite(s, 1, strlen(s), f), and
   printf("%s\n", s) to puts(s), when nothing reads the call's value.  It
   decided that by the `;` after the call, which `x = fputs(...);` and
   `int r = printf(...);` have too, so their value became a constant 0.

   YasOS libc's puts is `ret = fputs(s, stdout); ... return ret + 1;`, which
   then returned 1 for every string (ir_tests/112_builtin_puts on the device).

   Checked through printf, whose value (the number of characters written) is
   the same in every libc; fputs only promises "non-negative", and newlib's
   is 0, so a wrong 0 from fputs would not show here. */
#include <stdio.h>

int g;

__attribute__((noinline)) int printf_s_nl(const char *s)
{
  int n = printf("%s\n", s);
  return n;
}

__attribute__((noinline)) int printf_assign_global(const char *s)
{
  g = printf("%s\n", s);
  return g;
}

__attribute__((noinline)) int printf_in_expression(const char *s)
{
  return printf("%s\n", s) * 10;
}

/* The statement forms keep their lowering: nothing reads the value. */
__attribute__((noinline)) void statements(const char *s)
{
  printf("%s\n", s);
  fputs(s, stdout);
  fputs("\n", stdout);
}

int main(void)
{
  int a = printf_s_nl("four");
  int b = printf_assign_global("seven!!");
  int c = printf_in_expression("xy");
  statements("stmt");
  printf("printf_s_nl=%d printf_assign_global=%d printf_in_expression=%d\n", a, b, c);
  return (a == 5 && b == 8 && c == 30) ? 0 : 1;
}
