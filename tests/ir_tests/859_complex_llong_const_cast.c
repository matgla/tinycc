/* Casting an integer-complex constant to _Complex long long must keep both
 * parts (the packed 64-bit constant cannot hold two 64-bit elements; the
 * repacking shifted by 64 and OR-ed the parts together). */
int printf(const char *, ...);
typedef _Complex long long cll;
__attribute__((noinline)) static void show(const char *n, cll c)
{
  printf("%s=%lld %lld\n", n, __real__ c, __imag__ c);
}
int main(void)
{
  show("z", (cll)(1 + 2i));
  show("w", (cll)(_Complex short)(3 + 4i));
  show("n", (cll)(_Complex int)(-5 - 6i));
  return 0;
}
