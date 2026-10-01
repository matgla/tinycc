/* Regression: ra:load_postinc folded the loop's `++p` into the FIRST `*p` load
 * (`ldrb rX,[p],#1`) while a second read of P was still to come.  The second
 * read was an argument of the outer call: its FUNCPARAMVAL quad sits ABOVE the
 * load (params are emitted in source order, the nested call's load after
 * them), but the value is read at the outer CALL, below the load.  The scan
 * between the load and the add only looked at the quads in that window, so it
 * never saw the param and the outer call got p+1 -- "YASB" printed as
 * "A0S0B0\0 0".  FatFs f_setlabel hit it (sdformat: FR_INVALID_NAME).
 */
#include <stdio.h>

__attribute__((noinline)) int ext(int c) { return c == 'S'; }
__attribute__((noinline)) int ext16(int c) { return c == 300; }
__attribute__((noinline)) int take(const char *p, int v) { return *p * 2 + v; }

static void chars(const char *label)
{
  for (const char *p = label; *p; ++p)
    printf("%c%d", *p, ext(*p));
  printf("\n");
}

static int ptr_arg(const char *s)
{
  int sum = 0;
  for (const char *p = s; *p; ++p)
    sum += take(p, ext(*p));
  return sum;
}

static int shorts(const unsigned short *w)
{
  int sum = 0;
  for (const unsigned short *p = w; *p; ++p)
    sum = sum * 3 + printf("%d,%d ", *p, ext16(*p));
  printf("\n");
  return sum;
}

int main(void)
{
  static const unsigned short w[] = {100, 300, 7, 0};
  chars("YASB");
  printf("%d\n", ptr_arg("YASB"));
  printf("%d\n", shorts(w));
  return 0;
}
