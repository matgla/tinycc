extern unsigned ext_mix(unsigned);

static __attribute__((noinline)) unsigned dup(unsigned x)
{
  return ext_mix(x * 5u) + 7;
}

unsigned call_b(unsigned x)
{
  return dup(x);
}

unsigned spill_big(unsigned);
unsigned call_a(unsigned);

unsigned ext_mix(unsigned x)
{
  return x * 33u + 1;
}

void entry(void)
{
  volatile unsigned r = spill_big(1) + call_a(2) + call_b(3);
  (void)r;
}
