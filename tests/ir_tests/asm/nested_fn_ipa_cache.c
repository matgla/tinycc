/* File-scope functions keep their cached constant / switch results while a
 * nested function of the same name is called, not folded to them. */
#define NI __attribute__((noinline))

NI int r(int y) { return 1000000; }
NI int sw(int y)
{
  switch (y)
  {
  case 1: return 11;
  case 2: return 22;
  case 3: return 33;
  default: return 0;
  }
}
NI int k(void) { return 42; }

int use_r(void) { return r(2); }
int use_sw(void) { return sw(2); }
int use_k(void) { return k() + k(); }

int nested_r(void)
{
  NI int r(int y) { return y + 1000; }
  return r(2);
}
int nested_sw(void)
{
  NI int sw(int y)
  {
    switch (y)
    {
    case 1: return 101;
    case 2: return 202;
    case 3: return 303;
    default: return 7;
    }
  }
  return sw(2);
}
