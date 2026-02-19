/*
 * Bug: local variable 'expected' has correct value for comparison
 * but garbage value when passed as printf argument at -O1.
 * Reduced from bug_packed_sizes.c TEST_STRIDE macro.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct __attribute__((packed)) S3
{
  uint16_t a;
  uint8_t b;
} S3;

_Static_assert(sizeof(S3) == 3, "");

__attribute__((noinline)) int find_first_changed(uint8_t *raw, int len)
{
  for (int b = 0; b < len; b++)
  {
    if (raw[b] != 0xFF)
      return b;
  }
  return -1;
}

int main(void)
{
  int errors = 0;

  S3 pool[2];
  memset(pool, 0xFF, sizeof(pool));
  pool[1].a = 0;

  int off = find_first_changed((uint8_t *)pool, (int)sizeof(pool));
  int expected = (int)sizeof(S3);

  printf("off=%d expected=%d match=%d\n", off, expected, off == expected);

  if (off != expected)
  {
    printf("FAIL\n");
    errors++;
  }
  else
  {
    printf("OK\n");
  }

  return errors;
}
