#include <stdio.h>
#include <string.h>

int bench_strcpy(void)
{
  char src[256] = "The quick brown fox jumps over the lazy dog. "
                  "Pack my box with five dozen liquor jugs. "
                  "How vexingly quick daft zebras jump!";
  char dst[256];
  int len = 0;
  int iterations = 1000;

  for (int n = 0; n < iterations; n++) {
    strcpy(dst, src);
    len = strlen(dst);
  }

  return len;
}

int main(void)
{
    printf("strcpy: %d\n", bench_strcpy());
    return 0;
}
