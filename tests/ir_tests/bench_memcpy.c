#include <stdio.h>
#include <string.h>

int bench_memcpy(void)
{
  char src[512];
  char dst[512];
  int checksum = 0;
  int iterations = 1000;

  for (int i = 0; i < 512; i++) {
    src[i] = (char)((i * 7 + 13) & 0xFF);
  }

  for (int n = 0; n < iterations; n++) {
    memcpy(dst, src, 256);
    memcpy(dst + 256, src, 128);

    checksum = 0;
    for (int j = 0; j < 256; j++) {
      checksum += (unsigned char)dst[j];
    }
  }

  return checksum;
}

int main(void)
{
    printf("memcpy: %d\n", bench_memcpy());
    return 0;
}
