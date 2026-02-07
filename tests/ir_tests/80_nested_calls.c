/* Test nested function calls */

int add(int a, int b)
{
  return a + b;
}

int mul(int a, int b)
{
  return a * b;
}

int main(void)
{
  /* This creates nested call: add(mul(2, 3), mul(4, 5))
   * Inner calls must be evaluated before outer call arguments are set up */
  int result = add(mul(2, 3), mul(4, 5));
  /* Expected: add(6, 20) = 26 */
  return result;
}
