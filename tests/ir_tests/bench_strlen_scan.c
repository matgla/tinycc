#include <stdio.h>
#include <string.h>

int bench_strlen_scan(void)
{
  static const char *words[] = {
      "benchmark",
      "tinycc",
      "cortex-m33",
      "rp2350",
      "deterministic",
      "verification",
  };
  int total = 0;
  int iterations = 2000;

  for (int n = 0; n < iterations; n++) {
    total = 0;
    for (int i = 0; i < 6; i++) {
      total += (int)strlen(words[i]) * (i + 3);
    }
  }

  return total;
}

int main(void)
{
    printf("strlen_scan: %d\n", bench_strlen_scan());
    return 0;
}
