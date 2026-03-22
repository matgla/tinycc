extern int printf(const char *, ...);

/* Access float as uint32 for hex inspection */
static unsigned int float_bits(float f)
{
  union
  {
    float f;
    unsigned int u;
  } x;
  x.f = f;
  return x.u;
}

int main(void)
{
  _Complex float a;
  __real__ a = 2.0f;
  __imag__ a = 0.0f;

  _Complex float b;
  __real__ b = 0.0f;
  __imag__ b = 7.0f;

  printf("a.real=0x%08x a.imag=0x%08x\n", float_bits(__real__ a), float_bits(__imag__ a));
  printf("b.real=0x%08x b.imag=0x%08x\n", float_bits(__real__ b), float_bits(__imag__ b));

  _Complex float c = a + b;
  printf("c.real=0x%08x c.imag=0x%08x\n", float_bits(__real__ c), float_bits(__imag__ c));
  printf("c: %.1f + %.1fi\n", (double)__real__ c, (double)__imag__ c);

  return 0;
}
