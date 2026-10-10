#include <stdio.h>
__attribute__((noinline)) unsigned long long r64(unsigned long long x) { return (x << 8) | (x >> 24); }
__attribute__((noinline)) unsigned r32(unsigned x) { return (x << 8) | (x >> 24); }
int main(void)
{
  unsigned long long v = r64(0x0123456789abcdefull);
  printf("%08x%08x\n", (unsigned)(v >> 32), (unsigned)v);
  printf("%08x\n", r32(0x12345678u));
  return 0;
}
