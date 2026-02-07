#include <stdarg.h>
#include <stdio.h>

int main(void)
{
  printf("va_list_size=%u\n", (unsigned)sizeof(va_list));
  printf("PASS\n");
  return 0;
}
