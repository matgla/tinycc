/* Test IR optimizations */
int test_constant_prop()
{
  int i = 0; /* Constant variable */
  int *arr = 0;
  return arr[i]; /* Should optimize: i is constant 0 */
}

int test_algebraic()
{
  int x = 5;
  int y = x + 0; /* Should optimize to: y = x */
  int z = x * 1; /* Should optimize to: z = x */
  return y + z;
}

int test_cse()
{
  int a = 10;
  int b = 20;
  int x = a + b; /* Compute a + b */
  int y = a + b; /* Should reuse previous computation */
  return x + y;
}

int Move(int *source, int *dest)
{
  int i = 0, j = 0;
  while (j < 4 && dest[j] == 0)
    j++;
  dest[j - 1] = source[i];
  return dest[j - 1];
}
