/*
 * Minimal reproducer: array indexing with stride 10 (non-power-of-2).
 * Tests whether pool[i] correctly accesses element i of a 10-byte packed struct array.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct __attribute__((packed)) S10
{
  int32_t a; /* offset 0, 4 bytes */
  int32_t b; /* offset 4, 4 bytes */
  uint8_t c; /* offset 8, 1 byte */
  uint8_t d; /* offset 9, 1 byte */
} S10;

_Static_assert(sizeof(S10) == 10, "S10 must be 10 bytes");

int main(void)
{
  S10 pool[6];
  int errors = 0;

  /* Fill each element with its index as marker */
  for (int i = 0; i < 6; i++)
  {
    pool[i].a = i * 100;
    pool[i].b = i * 200;
    pool[i].c = i;
    pool[i].d = i + 10;
  }

  /* Verify each element can be read back */
  for (int i = 0; i < 6; i++)
  {
    int a = pool[i].a;
    int b = pool[i].b;
    int c = pool[i].c;
    int d = pool[i].d;
    if (a != i * 100 || b != i * 200 || c != i || d != i + 10)
    {
      printf("FAIL pool[%d]: a=%d b=%d c=%d d=%d (expected a=%d b=%d c=%d d=%d)\n", i, a, b, c, d, i * 100, i * 200, i,
             i + 10);
      errors++;
    }
    else
    {
      printf("OK pool[%d]: a=%d b=%d c=%d d=%d\n", i, a, b, c, d);
    }
  }

  /* Also test pointer arithmetic explicitly */
  printf("--- pointer arithmetic ---\n");
  for (int i = 0; i < 6; i++)
  {
    S10 *p = &pool[i];
    uintptr_t offset = (uintptr_t)p - (uintptr_t)pool;
    int expected_offset = i * 10;
    if ((int)offset != expected_offset)
    {
      printf("FAIL &pool[%d] offset=%d expected=%d\n", i, (int)offset, expected_offset);
      errors++;
    }
    else
    {
      printf("OK &pool[%d] offset=%d\n", i, (int)offset);
    }
  }

  if (errors == 0)
    printf("ALL PASSED\n");
  else
    printf("FAILED: %d errors\n", errors);
  return errors;
}
