#include <stdio.h>
#include <string.h>

int bench_strcmp(void)
{
  const char *s1 = "alpha";
  const char *s2 = "beta";
  int result = 0;
  int iterations = 1000;

  for (int n = 0; n < iterations; n++) {
    result = strcmp(s1, s2);
  }

  return result + 100;
}

int main(void)
{
    printf("strcmp: %d\n", bench_strcmp());
    return 0;
}
