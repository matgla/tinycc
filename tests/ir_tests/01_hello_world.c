#include <stdio.h>

int sum(int a, int b) { return a + b; }

extern char __end__;
extern unsigned int __heap_size__;

int main(int argc, char *argv[]) {
  puts("Hello world\n");
  int x = sum(3, 31);
  printf("Sum: %d, %x, %d\n", x, 123, 123);
  return x;
}