/* Chapter 1 -- hello, yasc (README.md).
 *
 *   tcc 01_hello.c -o hello      yasc is the default: no flag needed
 *
 * `#pragma yasc` makes this file yasc even under -ccompat (regular C), which
 * is how the test suite builds it.  In yasc every pointer says what it is:
 * here a view -- a pointer that borrows its target and never frees it. */
#pragma yasc
#include <stdio.h>

int main(void)
{
  const char *greeting = "hello, yasc";
  printf("%s\n", greeting);
  return 0;
}
