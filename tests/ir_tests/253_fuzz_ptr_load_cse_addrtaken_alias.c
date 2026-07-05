#include <stdio.h>

/*
 * Fuzz agg_deep seed 2265 reduction: pointer-load CSE must not reuse an
 * indirect read through **ppa across a direct write to address-taken u6.
 */
int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u3 = 2274096212u;
  unsigned u4 = 3068085536u;
  unsigned u5 = 2812471992u;
  unsigned u6 = 394840959u;
  char s1 = (char)(1100670913u & 0xff);
  unsigned *pa29 = &u6;
  unsigned **ppa210 = &pa29;
  unsigned g12 = 0u;

  while (g12 < 8u) {
    u6 = (((u3 + u5) ^ ((u6 & 1u) ? 2415012122u : (**ppa210))) ^
          (u5 / (((**ppa210) | ((**ppa210) ^ cs)) | 1u))) ^
         (**ppa210);
    u6 = (**ppa210) +
         (((((u4 % (1501913368u | 1u)) | (u5 >= ((**ppa210) ^ cs)))) & 1u)
              ? (unsigned)s1
              : 3908257788u);
    cs = cs * 2654435761u;
    g12++;
  }

  printf("checksum=%08x\n", u6);
  return 0;
}
