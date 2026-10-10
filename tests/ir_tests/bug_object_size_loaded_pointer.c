/* __builtin_object_size of a pointer loaded from a local array/struct used the
 * container's size.  Modes 2/3 returned -1 for unknown (must be 0). */
#include <stdio.h>

struct S { char *p; int n; };

__attribute__((noinline)) void f(char *buf)
{
  char *ptrs[2];
  struct S s;
  char loc[10];
  ptrs[1] = buf;
  s.p = buf;
  printf("%d %d\n", (int)__builtin_object_size(ptrs[1], 0),
         (int)__builtin_object_size(s.p, 0));
  printf("%d %d %d\n", (int)__builtin_object_size(loc, 0),
         (int)__builtin_object_size(loc, 2), (int)__builtin_object_size(loc + 3, 0));
  printf("%d %d\n", (int)__builtin_object_size(ptrs[1], 2),
         (int)__builtin_object_size(s.p, 3));
}

int main(void)
{
  static char big[100];
  f(big);
  return 0;
}
