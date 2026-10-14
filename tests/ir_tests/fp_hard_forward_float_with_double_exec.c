/* Hard-float: forwarding a VFP-resident float into a call whose first VFP
 * argument is a double (which occupies d0 = s0/s1) must not clobber the float
 * before it is moved. */
__attribute__((noinline)) float g(double d, float f) { return f + (float)(d > 1.0); }
__attribute__((noinline)) float caller(float f, double d) { return g(d, f); }
__attribute__((noinline)) float g2(double a, double b, float f, float h) { return f * 2.0f + h + (float)(a > b); }
__attribute__((noinline)) float caller2(float f, float h, double a, double b) { return g2(a, b, h, f); }

int main(void)
{
  volatile float f = 5.0f, h = 3.0f;
  volatile double d = 7.0, e = 2.0;
  if (caller(f, d) != 6.0f)
    return 10;
  if (caller2(f, h, d, e) != 3.0f * 2.0f + 5.0f + 1.0f)
    return 11;
  return 1;
}
