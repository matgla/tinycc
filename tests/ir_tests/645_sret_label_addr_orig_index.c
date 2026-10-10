/* A function that takes a label's address has every struct-returning call
   routed through a fresh temporary and copied out after the call
   (tcc_ir_sret_dealias).  The copy is a new instruction: codegen maps each
   instruction's orig_index to its code address and &&label reads that map,
   so a copy that kept orig_index 0 moved a label on the first instruction to
   the copy after the call -- `goto *tab[0]` skipped the increment and the
   call and looped forever (-O0 and -O2). */
#include <stdio.h>

typedef struct
{
  int a[6];
} S;

__attribute__((noinline)) S mk(int k)
{
  S r;
  for (int i = 0; i < 6; i++)
    r.a[i] = k + i;
  return r;
}

static int st;

__attribute__((noinline)) int run(int n)
{
  static void *tab[] = {&&top, &&done};
  S acc;
top:
  st++;
  acc = mk(st);
  if (st < n)
    goto *tab[0];
  goto *tab[1];
done:
  return acc.a[0] + acc.a[5] + acc.a[2] + st;
}

int main(void)
{
  int r = run(3);
  printf("%d %d\n", r, st);
  return 0;
}
