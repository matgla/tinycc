// #include <stdio.h>

// int main() {
//   int Count;
//   int Array[10];

//   for (Count = 1; Count <= 10; Count++) {
//     Array[Count - 1] = Count * Count;
//   }

//   for (Count = 0; Count < 10; Count++) {
//     printf("%d\n", Array[Count]);
//   }

//   return 0;
// }

// // vim: set expandtab ts=4 sw=3 sts=3 tw=80 :

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