/* Built by arm-none-eabi-gcc (test_yasc_src_gcc_interop): reads a yasc_src
 * tcc made, and makes one for tcc to read -- the same layout both ways. */
#include <stdio.h>
#include <string.h>
#include <yasc_src.h>

static const char *base(const char *p)
{
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

void gcc_show(const char *what, yasc_src at)
{
  printf("gcc %s %s:%u %s\n", what, base(YASC_SRC_FILE(at)), at.line, YASC_SRC_FUNC(at));
}

yasc_src gcc_here(void)
{
  return YASC_SRC;
}
