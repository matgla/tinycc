#include <stdio.h>

int bench_switch(void)
{
  int r = 0;
  int iterations = 1000;

  for (int n = 0; n < iterations; n++) {
    int i = 7;

    r = 1000;

    switch (i) {
    case 0: r += i + 1; break;
    case 1: r -= i; break;
    case 2: r *= 2; r /= 2; r += 1; break;
    case 3: r = r / 2 + 1; break;
    case 4: r ^= i; break;
    case 5: r &= (0xFFFF + i); break;
    case 6: r |= (i & 0x0F); break;
    case 7: r = (r ^ 0xFF) ^ 0xFF; break;
    }
  }

  return r;
}

int main(void)
{
    printf("switch_stmt: %d\n", bench_switch());
    return 0;
}
