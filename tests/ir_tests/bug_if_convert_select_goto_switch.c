/* if_convert's SELECT diamond assumed the else block follows the then-JUMP and
 * counted only JUMP/JUMPIF predecessors: a goto into the arm (f) or a switch
 * case inside the arm (g) lost the arm's assignment. */
#include <stdio.h>

__attribute__((noinline)) int f(int a, int b)
{
  int t;
  if (b > 5)
    goto X;
  if (a) {
    t = 1;
    goto M;
  X:
    b++;
  }
  t = 2;
M:
  return t + b;
}

int log_[8];
int nl;
__attribute__((noinline)) void use(int a, int b) { log_[nl++] = a * 100 + b; }

__attribute__((noinline)) int g(int x, int y)
{
  int r = 7;
  switch (x) {
  case 0:
    if (y) {
      r = 1;
    } else {
  case 1:
      r = 2;
    }
    break;
  case 2: use(1, y); break;
  case 3: use(2, y); break;
  case 4: use(3, y); break;
  case 5: use(4, y); break;
  }
  return r;
}

int main(void)
{
  printf("%d %d | %d %d %d %d\n", f(1, 0), f(0, 0), g(1, 0), g(0, 1), g(0, 0), g(9, 0));
  return 0;
}
