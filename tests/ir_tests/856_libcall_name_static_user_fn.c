/* A static function that reuses a libm/libc name is the program's own, not the
 * library's: calls with constant arguments must not be folded as libc. */
int printf(const char *, ...);
static int round(int x) { return x * 10 + 1; }
static double exp(double x) { return x + 1000.0; }
static unsigned strlen(const char *s) { (void)s; return 42; }
static int strcmp(const char *a, const char *b) { (void)a; (void)b; return 7; }
int main(void)
{
  printf("%d %d\n", round(5), (int)exp(1.0));
  printf("%u %d\n", strlen("abc"), strcmp("x", "x"));
  return 0;
}
