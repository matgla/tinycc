#include <stdio.h>
#include <string.h>

void __attribute__((noinline)) show(const char *b)
{
  for (int i = 0; i < 20; i++)
    putchar(b[i] ? b[i] : '.');
  putchar('\n');
}

/* strcpy to an odd stack offset must not become a word LDM/STM block copy. */
int main(void)
{
  char buf[24];
  memset(buf, 'x', sizeof buf);
  strcpy(buf + 1, "abcdefghijk");
  strcpy(buf + 14, "LMN");
  show(buf);
  return 0;
}
