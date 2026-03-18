extern inline int add1_inline(int x)
{
  asm("adds %0, %0, #1" : "+r"(x));
  return x;
}

int main(void)
{
  return add1_inline(41) != 42;
}