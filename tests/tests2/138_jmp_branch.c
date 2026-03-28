#include <stdio.h>

static void tst_branch(void)
{
  printf("tst_branch --");
  goto *&&a;
  printf(" dummy");
a:
  printf(" --\n");
}

int main(void)
{
  tst_branch();
  return 0;
}