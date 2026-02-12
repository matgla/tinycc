/* Bug: switch on char with sparse, non-contiguous case values.
 *
 * TCC ARM codegen emits the switch dispatch code but fails to emit
 * the case bodies entirely.  The generated code reads the switch
 * variable, then jumps straight back to the loop top, skipping all
 * case handlers.
 *
 * Reproduces the vfprintf miscompilation where %s/%d/etc. format
 * specifiers produce no output.
 */
#include <stdio.h>

int classify(int c)
{
  int r = -1;
  switch (c)
  {
  case 'd':
  case 'i':
    r = 1;
    break;
  case 'u':
    r = 2;
    break;
  case 'o':
    r = 3;
    break;
  case 'p':
    r = 40;
    /* fallthrough */
  case 'x':
    r += 4;
    break;
  case 'X':
    r = 5;
    break;
  case 'c':
    r = 6;
    break;
  case 's':
    r = 7;
    break;
  case 'n':
    r = 8;
    break;
  case '\0':
    r = 0;
    break;
  default:
    r = 99;
    break;
  }
  return r;
}

int main(void)
{
  printf("d -> %d\n", classify('d'));
  printf("i -> %d\n", classify('i'));
  printf("u -> %d\n", classify('u'));
  printf("o -> %d\n", classify('o'));
  printf("x -> %d\n", classify('x'));
  printf("X -> %d\n", classify('X'));
  printf("c -> %d\n", classify('c'));
  printf("s -> %d\n", classify('s'));
  printf("n -> %d\n", classify('n'));
  printf("p -> %d\n", classify('p'));
  printf("0 -> %d\n", classify('\0'));
  printf("z -> %d\n", classify('z'));
  return 0;
}
