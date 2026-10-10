/* Second file of 721_tu_function_caches.c: g0..g3 are interned first here,
 * taking the token numbers f0..f3 had in the first file. */
int g0(void);
int g1(void);
int g2(void);
int g3(void);
int f0(void);
int printf(const char *, ...);

int main(void)
{
  printf("%d %d %d %d\n", g0(), g1(), g2(), g3());
  printf("%d\n", f0());
  return 0;
}
