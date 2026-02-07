#include <stdio.h>

typedef union {
  unsigned long long ull;
  struct { unsigned lo; unsigned hi; } s;
} U64;

int main(void) {
  long long v = 1;
  for (int i = 0; i < 5; i++) {
    U64 u;
    u.ull = (unsigned long long)v;
    printf("%d: hi=%08x lo=%08x\n", i, u.s.hi, u.s.lo);
    v *= 10;
  }
  return 0;
}
