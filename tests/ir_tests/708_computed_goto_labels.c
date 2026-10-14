#include <stdio.h>

/* Computed goto: a label on an instruction a pass deleted (the unrolled
 * loop's init) resolved to the next ORIGINAL index -- past the unrolled
 * copies, which carry fresh ones; and at -O0 the indirect jump's scratch
 * register was popped after the branch, never. */
int arr[4];
static int st;
__attribute__((noinline)) int run(int n)
{
  static void *tab[] = {&&top, &&mid, &&done};
  int s = 0;
top:
  st++;
  if (st & 1) goto *tab[1];
  s += 100;
mid:
  for (int i = 0; i < 4; i++) arr[i] += st;
  if (st < n)
    goto *tab[0];
  goto *tab[2];
done:
  return s;
}

int arr2[4];
__attribute__((noinline)) int run2(int n)
{
  static void *tab[] = {&&mid, &&done};
  int st2 = 0;
  goto *tab[0];
mid:
  st2++;
  for (int i = 0; i < 4; i++) arr2[i] += st2;
  if (st2 < n)
    goto *tab[0];
  goto *tab[1];
done:
  return st2;
}

int main(void)
{
  int r = run(5);
  printf("%d %d %d %d\n", r, st, arr[0], arr[3]);
  r = run2(3);
  printf("%d %d %d\n", r, arr2[0], arr2[3]);
  return 0;
}
