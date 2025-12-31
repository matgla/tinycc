#include <stdio.h>

float global_float = 1.5f;

int main()
{
  float a = 1.0f;
  float b = 2.0f;
  float c = a + b + global_float;

  if (c > 2.5f)
  {
    printf("Float addition works: %f + %f = %f\n", a, b, c);
    return 1;
  }
  return 0;
}
