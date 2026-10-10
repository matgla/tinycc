#include <stdio.h>
#include <string.h>

/* Frame relayout runs on functions with inline asm.  The asm is lowered from
 * the operand SValues the front end saved, which no IR rewrite reaches: a
 * slot one of them names by frame offset ("+r"(arr[1])) stays where it is,
 * while dead objects go and disjoint ones share bytes around it.  A pointer
 * handed to the asm ("r"(&arr[2])) lets its object escape. */

struct big { int a[40]; };
struct pair { int x, y; };

__attribute__((noinline)) int use(struct big *b) { return b->a[3] + b->a[39]; }

__attribute__((noinline)) int f(int v)
{
  struct pair s;
  struct big t1, t2, dead;
  int arr[4];
  int r, p;
  s.x = v;
  s.y = v + 1;
  arr[0] = arr[2] = arr[3] = 7;
  arr[1] = v;
  memset(&t1, v, sizeof t1);
  r = use(&t1);
  asm volatile("adds %0, %0, #1" : "+r"(s.x) : : "cc");
  asm volatile("adds %0, %0, #2" : "+r"(arr[1]) : : "cc");
  memset(&t2, v + 2, sizeof t2);
  r += use(&t2);
  asm volatile("ldr %0, [%1]" : "=r"(p) : "r"(&arr[2]) : "memory");
  (void)dead;
  return r + s.x * 10 + s.y * 100 + arr[1] * 1000 + p * 10000 + arr[0] + arr[3];
}

int main(void)
{
  printf("%d\n", f(1));
  printf("%d\n", f(3));
  return 0;
}
