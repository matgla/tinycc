/* Block-scoped objects may share frame bytes once their blocks are disjoint
 * (tcc_ir_frame_scope_block): their lifetime is the block, whatever their
 * address did.  What must still keep them apart is a value carried around a
 * loop whose back edge stays inside an object's block -- through a pointer,
 * from a later iteration back to an earlier point -- while another block's
 * object is written in between. */
#include <stdio.h>

__attribute__((noinline)) void fill(int *a, int n, int v)
{
  for (int i = 0; i < n; i++)
    a[i] = v + i;
}

__attribute__((noinline)) int sum(const int *a, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += a[i];
  return s;
}

/* Disjoint blocks inside a loop: each case's array is dead outside it. */
int cases(int n)
{
  int total = 0;
  for (int i = 0; i < n; i++)
  {
    switch (i % 3)
    {
    case 0:
    {
      int a[16];
      fill(a, 16, i);
      total += sum(a, 16);
      break;
    }
    case 1:
    {
      int b[16];
      fill(b, 16, 2 * i);
      total += sum(b, 16) ^ 7;
      break;
    }
    default:
    {
      int c[16];
      fill(c, 16, 3 * i);
      total -= sum(c, 16);
      break;
    }
    }
  }
  return total;
}

/* x's first reference is inside the loop, after y's block: x lives in the
 * enclosing block, so its value carries to the next iteration through p while
 * y is written again -- they must not share. */
int carried(int n)
{
  int x;
  int *p = 0;
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    {
      int y[8];
      fill(y, 8, 1000 * i);
      s += sum(y, 8);
    }
    if (!p)
    {
      p = &x;
      fill(p, 1, 1);
    }
    else
      fill(p, 1, *p + 10);
  }
  return s + x;
}

/* A backward goto inside one block: x (declared after the label) lives for
 * the whole block, so the value read through p at the top of the next round
 * is the one stored before the goto, although y was written since. */
int goto_back(void)
{
  int r = 0, n = 0;
  int *p = 0;
  {
  again:
    {
      int y[8];
      fill(y, 8, 7 * n);
      r += sum(y, 8);
    }
    if (p)
      r += *p;
    int x;
    fill(&x, 1, 50 + n);
    p = &x;
    if (++n < 4)
      goto again;
  }
  return r;
}

int main(void)
{
  printf("cases %d\n", cases(10));
  printf("carried %d\n", carried(5));
  printf("goto_back %d\n", goto_back());
  return 0;
}
