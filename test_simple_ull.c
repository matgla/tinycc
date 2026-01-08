/* Simple test for unsigned long long subtraction */

int printf(const char *, ...);

int main(void)
{
  volatile unsigned long long ull = 1;

  /* This subtracts 100 from a 64-bit value */
  unsigned long long result = ull - 100;

  printf("result: %llu\n", result);

  return 0;
}
