/* Test complex unsigned integer division - isolated checks */

unsigned char g;

__attribute__((noinline)) unsigned char bar(_Complex unsigned c)
{
  unsigned char v = g;
  _Complex unsigned t = 42;
  t /= c;
  return v + t;
}

int main()
{
  unsigned char y = bar(7);
  return y; /* Should be 6 */
}
