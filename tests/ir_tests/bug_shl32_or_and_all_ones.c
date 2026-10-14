#include <stdio.h>

__attribute__((noinline))
unsigned long long a3(unsigned a, unsigned long long b) {
  unsigned long long m = ~0ull;
  return (((unsigned long long)a << 32) | b) & m;
}

__attribute__((noinline))
unsigned long long a4(unsigned a, unsigned long long b) {
  unsigned long long m = 0xFFFFFFFFull;
  return (((unsigned long long)a << 32) | b) & m;
}

int main(void) {
  printf("%llx\n", a3(0x12345678u, 0x9abcdef0ull));
  printf("%llx\n", a4(0x12345678u, 0x9abcdef0ull));
  return 0;
}
