/* The linker folds each function's section into .text, frees it and renumbers
 * what is left before .text is allocated, and its per-symbol attributes are in
 * blocks of 1024.  Every function here has its own section and a value only it
 * returns, so a symbol or relocation left on an old number calls the wrong one.
 * Reached directly and through tables in data and rodata. */
#include <stdio.h>

static int bias = 1;
static const int scale[4] = {3, 5, 7, 11};

#define F(n) int f##n(int a) { return a + n + bias * scale[n & 3]; }
#define T10(p) F(p##0) F(p##1) F(p##2) F(p##3) F(p##4) F(p##5) F(p##6) F(p##7) F(p##8) F(p##9)
#define T100(p) T10(p##0) T10(p##1) T10(p##2) T10(p##3) T10(p##4) T10(p##5) T10(p##6) T10(p##7) T10(p##8) T10(p##9)

/* f1000-f2099: past one attribute block, and no more than that: this compiles on
 * the target too, in seconds per thousand functions. */
T100(10) T100(11) T100(12) T100(13) T100(14) T100(15) T100(16) T100(17) T100(18) T100(19)
T100(20)

typedef int (*fn)(int);
static fn writable[] = {f1000, f1234, f1999, f2000, f2050, f2099};
static fn const fixed[] = {f1001, f1777, f2001, f2098};

int main(void)
{
  long sum = 0;
  unsigned i;
  for (i = 0; i < sizeof writable / sizeof *writable; i++)
    printf("%d\n", writable[i](1));
  for (i = 0; i < sizeof fixed / sizeof *fixed; i++)
    printf("%d\n", fixed[i](2));
  sum = f1500(0) + f1501(0) + f2075(0) + f2076(0);
  printf("%ld\n", sum);
  return 0;
}
