/* Inline const-eval must not fold a non-const static element to its initializer.
 *
 * indir() and try_inline_const_eval() treated possibly_written == 0 as "never
 * written", but the flag is parse-order and misses writes through an escaped
 * pointer (static initializer `int *p = tab`) and writes in later functions.
 * Fix: the frontend folds only const-qualified objects; the IR global_init_prop
 * pass folds the rest in the end-of-TU late_reopt phase. */
#include <stdio.h>

static int tab[2] = {5, 6};
int *ptab = tab;
__attribute__((noinline)) void sett(void) { ptab[1] = 9; }
static int geti(int i) { return tab[i]; }
__attribute__((noinline)) int use3(void) { return geti(1); }

static int tab2[2] = {5, 6};
static int geti2(int i) { return tab2[i]; }
__attribute__((noinline)) int use4(void) { return geti2(1); }
__attribute__((noinline)) void sett2(void) { tab2[1] = 9; }

static int sc = 7;
static int getsc(void) { return sc; }
__attribute__((noinline)) int use5(void) { return getsc(); }
__attribute__((noinline)) void setsc(void) { sc = 11; }

static const int ctab[2] = {3, 4};
static int getc1(int i) { return ctab[i]; }
__attribute__((noinline)) int use6(void) { return getc1(1); }

static int never[2] = {20, 21};
static int getn(int i) { return never[i]; }
__attribute__((noinline)) int use7(void) { return getn(1); }

int main(void)
{
  printf("before %d %d %d %d %d\n", use3(), use4(), use5(), use6(), use7());
  sett();
  sett2();
  setsc();
  printf("after %d %d %d %d %d\n", use3(), use4(), use5(), use6(), use7());
  return 0;
}
