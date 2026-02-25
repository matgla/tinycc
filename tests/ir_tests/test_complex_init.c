#include <stdio.h>

int main(void)
{
  _Complex float a = 1.0f;
  float real = __real__ a;
  float imag = __imag__ a;
  printf("a = %.1f + %.1fi\n", real, imag);
  return 0;
}
