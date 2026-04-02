#include <stdio.h>

int bench_bitwise_mix(void)
{
  unsigned int value = 0x13579BDFu;
  int iterations = 1000;

  for (int n = 0; n < iterations; n++) {
    value ^= value << 7;
    value ^= value >> 9;
    value += 0x9E3779B9u;
    value = (value << 3) | (value >> 29);
  }

  return (int)(value & 0x7FFFFFFFu);
}

int main(void)
{
    printf("bitwise_mix: %d\n", bench_bitwise_mix());
    return 0;
}
