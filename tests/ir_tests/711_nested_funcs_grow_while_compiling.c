/* More nested functions than the table's first allocation (4), with the
 * fifth and later discovered while a nested function is being compiled: the
 * table was reallocated under compile_nested_functions, which kept reading
 * the entry it was compiling (and parent_nf links) from the freed array --
 * a use-after-free that segfaulted the compiler on the second outer(). */
#include <stdio.h>

int outer(int n)
{
  int a = n, b = n + 1;
  int f1(int x) { return x + a; }
  int f2(int x) { return x + b; }
  int f3(int x) { return x * a; }
  int f4(int x) { int g(int y) { return y + x + a; } return g(x) + b; }
  return f1(1) + f2(2) + f3(3) + f4(4);
}

int outer2(int n, int m, int q8, int q9)
{
  int v0 = n + 0, v1 = n + 1, v3 = n + 3, v15 = n + 15, v16 = n + 16;
  int v17 = n + 17, v27 = n + 27, v28 = n + 28, v29 = n + 29;
  int vla[n];
  for (int i = 0; i < n; i++)
    vla[i] = i + 7;
  int f1(int x) { int l0 = x, l1 = x + 1; return l0 + l1 + v29 + q9 + vla[0]; }
  int f2(int x)
  {
    int g(int y)
    {
      int z = y + v3;
      int h(int w) { return w + v28 + q8 + z; }
      return h(z) + v27;
    }
    return g(x) + v1;
  }
  int f3(int x) { return x + v15 + v16 + v17 + m; }
  int f4(int x) { int k(int y) { return y * v0 + m; } return k(x) - v1; }
  int f5(int x) { return x - v3; }
  return f1(1) + f2(2) + f3(3) + f4(4) + f5(5) + v0;
}

int main(void)
{
  printf("outer %d %d\n", outer(5), outer(-2));
  printf("outer2 %d %d\n", outer2(3, 4, 100, 1000), outer2(6, -1, 7, 8));
  return 0;
}
