/* Hard-float: unused double parameters must not emit an UNDEFINED vmov in the
 * prologue (the dead parameter has no register; its allocation is 0xFFFF). */
__attribute__((noinline)) double first(double x, double y) { return x; }
__attribute__((noinline)) double second(double x, double y) { return y; }
__attribute__((noinline)) double none(double x, double y) { return 7.0; }
__attribute__((noinline)) int mixed(int n, double x, float f, double y) { return n + (int)f; }

int main(void)
{
  volatile double a = 3.0, b = 4.0;
  volatile float f = 5.0f;
  if (first(a, b) != 3.0) return 2;
  if (second(a, b) != 4.0) return 3;
  if (none(a, b) != 7.0) return 4;
  if (mixed(10, a, f, b) != 15) return 5;
  return 1;
}
