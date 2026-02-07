/* Test to dump what printf receives */
#include <stdarg.h>
#include <stdio.h>

/* Custom printf-like that shows what it receives */
void myprintf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);

  /* Read the double properly */
  double d = va_arg(ap, double);
  unsigned int *p = (unsigned int *)&d;

  va_end(ap);

  printf("myprintf got: lo=0x%08x hi=0x%08x val=%f\n", p[0], p[1], d);
}

int main()
{
  myprintf("test", 2.6);
  printf("Printf shows: %f\n", 2.6);
  return 0;
}
