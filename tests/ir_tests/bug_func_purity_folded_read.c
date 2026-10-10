/* tcc_ir_infer_func_purity called a function CONST when its read of memory was
 * folded into a non-LOAD operand (CMP/ADD with an lval source), and LICM hoisted
 * the call out of a loop that rewrites the memory. */
#include <stdio.h>

volatile int vn = 4, vk = 2;
int G;

__attribute__((noinline)) static int rd(int x) { return G == x; }
__attribute__((noinline)) static int rg(int x) { return G + x; }
__attribute__((noinline)) static int rp(int *p) { return *p == 3; }

__attribute__((noinline)) int lb(int n, int k)
{
  int c = 0, i = 0;
  do {
    G = i;
    c += rd(k);
    i++;
  } while (i < n);
  return c;
}

__attribute__((noinline)) int lb2(int n, int k)
{
  int c = 0, i = 0;
  do {
    G = i;
    c += rg(k);
    i++;
  } while (i < n);
  return c;
}

__attribute__((noinline)) int lb3(int n, int *p, int *q)
{
  int c = 0, i = 0;
  do {
    *q = i;
    c += rp(p);
    i++;
  } while (i < n);
  return c;
}

int main(void)
{
  int cell = 0;
  printf("lb %d lb2 %d lb3 %d\n", lb(vn, vk), lb2(vn, vk), lb3(vn, &cell, &cell));
  return 0;
}
