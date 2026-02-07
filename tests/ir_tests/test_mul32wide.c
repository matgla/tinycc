#include <stdint.h>
#include <stdio.h>

int main(void)
{
  uint32_t a = 0x12345678U;
  uint32_t b = 0x9abcdef0U;
  uint64_t r = (uint64_t)a * (uint64_t)b;
  printf("mul32wide=0x%llx\n", (unsigned long long)r);
  printf("PASS\n");
  return 0;
}
