#include <stdio.h>

/* GCC PR c/2735: union self-casts must still work when spelled through a
 * typedef alias.  */
union u
{
  int i;
};

typedef union u uu;

union u a;
uu b;

int main(void)
{
  b.i = 11;
  a = (union u)b;
  printf("a=%d\n", a.i);

  a.i = 22;
  b = (uu)a;
  printf("b=%d\n", b.i);

  b = (union u)a;
  printf("b2=%d\n", b.i);

  a = (uu)b;
  printf("a2=%d\n", a.i);

  return 0;
}