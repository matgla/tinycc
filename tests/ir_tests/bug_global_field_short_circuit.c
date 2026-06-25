#include <stdio.h>

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
  TT.show_process = mark_called;
  TT.threadparent = 0;
  printf("%d %d %d\n", test_gate((void *)1), TT.called, TT.kcount);

  TT.threadparent = (void *)1;
  printf("%d %d %d\n", test_gate((void *)1), TT.called, TT.kcount);

  TT.show_process = 0;
  TT.threadparent = 0;
  printf("%d %d %d\n", test_gate((void *)1), TT.called, TT.kcount);

  return 0;
}
