#include <stddef.h>

__attribute__((__noinline__)) int strncmp(const char *s1, const char *s2, size_t n)
{
  const unsigned char *u1 = (const unsigned char *)s1;
  const unsigned char *u2 = (const unsigned char *)s2;

  if (n == 0)
    return 123;

  while (n > 0)
  {
    unsigned char c1 = *u1++;
    unsigned char c2 = *u2++;
    if (c1 == '\0' || c1 != c2)
      return c1 - c2;
    n--;
  }

  return 0;
}

int main(void)
{
  const char *const s1 = "hello world";
  const char *s2 = s1;
  const char *s3 = s1 + 4;

  if (strncmp(++s2, ++s3, 0) != 0)
    return 1;
  if (s2 != s1 + 1)
    return 2;
  if (s3 != s1 + 5)
    return 3;

  return 0;
}