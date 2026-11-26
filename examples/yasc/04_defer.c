/* Chapter 4 -- defer (README.md).
 *
 * `defer STMT;` runs STMT whenever the scope is left -- falling off its end,
 * return, break, continue -- last registered first.  It puts the cleanup next
 * to what needs it.  Build: tcc 04_defer.c mem.o -o defer */
#pragma yasc
#include <stdio.h>
#include "mem.h"

static void note(const char *what)
{
  printf("  %s\n", what);
}

static int work(int fail_at)
{
  printf("work(%d)\n", fail_at);
  defer note("cleanup 1");
  if (fail_at == 1)
    return -1;
  defer note("cleanup 2"); /* runs before cleanup 1 */
  if (fail_at == 2)
    return -2;
  note("done");
  return 0;
}

/* The buffer is freed on every way out, without repeating mem_free. */
static int shout(const char *s)
{
  char * owned maybe buf = mem_strdup(s);
  if (!buf)
    return -1;
  defer mem_free(buf);
  for (char * view p = buf; *p; p++)
    if (*p >= 'a' && *p <= 'z')
      *p = (char)(*p - 'a' + 'A');
  if (buf[0] == 'Q')
    return 1; /* still freed */
  printf("%s\n", buf);
  return 0;
}

int main(void)
{
  for (int i = 0; i < 3; i++)
    work(i);
  shout("hello");
  shout("quiet");
  for (int i = 0; i < 3; i++)
  {
    defer note("end of iteration");
    if (i == 1)
      continue;
    printf("iteration %d\n", i);
  }
  printf("blocks left: %d\n", mem_live());
  return 0;
}
