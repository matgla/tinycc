#include <stdio.h>

struct S
{
  unsigned ub : 5;
} s;

int main()
{
  s.ub = 15;
  printf("Direct: %d\n", +s.ub);
  printf("Cast: %d\n", +(unsigned)s.ub);
  return 0;
}
