#include <stdio.h>

int sum(int a, int b)
{
  return a + b;
}

int main(int argc, char *argv[])
{
  puts("Hello world\n");
  int x = sum(3, 31);
  printf("Sum: %d, %x, %d, %x\n", x, 123, 123, 0xdead);
  return x;
}