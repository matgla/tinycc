#include <stdio.h>

/* Regression test for && short-circuit evaluation when both operands are
 * fields of a static global struct.  The previous revision observed the
 * effect through printf arguments, whose evaluation order is unspecified
 * (gcc reads the globals before the call, tcc after); this version uses
 * sequence points so the expected output is well-defined. */

struct state
{
  int kcount;
  void (*show_process)(void *);
  void *threadparent;
  int called;
};

static struct state TT;

static void mark_called(void *ptr)
{
  if (ptr)
    TT.called++;
}

static int test_gate(void *tb)
{
  TT.kcount++;
  if (TT.show_process && !TT.threadparent)
  {
    TT.show_process(tb);
    return 0;
  }

  return 1;
}

int main(void)
{
  int r;

  TT.show_process = mark_called;
  TT.threadparent = 0;
  r = test_gate((void *)1);
  printf("case1: gate=%d called=%d kcount=%d\n", r, TT.called, TT.kcount);

  TT.threadparent = (void *)1;
  r = test_gate((void *)1);
  printf("case2: gate=%d called=%d kcount=%d\n", r, TT.called, TT.kcount);

  TT.show_process = 0;
  TT.threadparent = 0;
  r = test_gate((void *)1);
  printf("case3: gate=%d called=%d kcount=%d\n", r, TT.called, TT.kcount);

  return 0;
}
