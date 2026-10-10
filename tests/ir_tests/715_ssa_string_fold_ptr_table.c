#include <stdio.h>
#include <string.h>

/* A pointer read from a rodata pointer table is not itself a string: the
 * bytes of the table slot (a relocation placeholder) must never be folded. */
const char *const tab[] = {"abc", "defg"};

int main(void)
{
  const char *const *t = tab;
  printf("%d %d %d\n", (int)strlen(t[1]), strcmp(t[0], "abc"), memchr(t[1], 'g', 4) != 0);
  return 0;
}
