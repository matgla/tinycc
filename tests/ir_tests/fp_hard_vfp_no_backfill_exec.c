/* Hard-float: once a VFP argument went to the stack (no even pair left for the
 * ninth double) no later float back-fills the free s1 -- it goes on the stack
 * too (AAPCS C.2.vfp).  The callee used to read it from s1 while the caller
 * stored it on the stack. */
__attribute__((noinline)) float pick(float a, double d1, double d2, double d3, double d4, double d5, double d6,
                                     double d7, double d8, float b)
{
  return b + (float)(d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8) * 0.0f + a * 0.0f;
}

/* the same with the early parameters unused */
__attribute__((noinline)) float pick2(float a, double d1, double d2, double d3, double d4, double d5, double d6,
                                      double d7, double d8, float b, float c)
{
  (void)a;
  (void)d1;
  (void)d2;
  (void)d3;
  (void)d4;
  (void)d5;
  (void)d6;
  (void)d7;
  (void)d8;
  return b * 10.0f + c;
}

int main(void)
{
  float r = pick(1.0f, 1, 2, 3, 4, 5, 6, 7, 8.0, 42.0f);
  if (r != 42.0f)
    return 2;
  float r2 = pick2(1.0f, 1, 2, 3, 4, 5, 6, 7, 8.0, 4.0f, 2.0f);
  if (r2 != 42.0f)
    return 3;
  return 1;
}
