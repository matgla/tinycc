#include <stdio.h>
#include <string.h>
__attribute__((noinline)) static int hit(int c) { return !!strchr("|)", c); }
__attribute__((noinline)) static int hit2(char *s) { return !!strchr("{(", *s); }
int main(void)
{
  volatile char z = 0, a = 'a', p = ')';
  char buf[2] = {0, 0};
  printf("%d %d %d %d %d\n", hit(z), hit(a), hit(p), hit2(buf), !!strchr("xy", z));
  return 0;
}
