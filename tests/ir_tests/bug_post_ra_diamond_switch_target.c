/* post_ra_forward_diamond NOPed a JUMP that a switch table targets
 * (`case 3: break;`), so the case fell into the other arm. */
#include <stdio.h>

int log_[8];
int nl;
__attribute__((noinline)) void use(int a, int b) { log_[nl++] = a * 100 + b; }

__attribute__((noinline)) void g(int x, int y)
{
  switch (x) {
  case 0:
    if (y)
      goto T;
  case 3:
    break;
  case 4:
  T:
    use(5, y);
    break;
  case 1: use(2, y); break;
  case 2: use(3, y); break;
  case 5: use(4, y); break;
  }
  use(9, 9);
}

int main(void)
{
  static const int xs[] = {3, 0, 4, 1, 6};
  for (int i = 0; i < 5; i++) {
    nl = 0;
    g(xs[i], 1);
    printf("x=%d n=%d %d %d\n", xs[i], nl, log_[0], nl > 1 ? log_[1] : 0);
  }
  return 0;
}
