/* tcc's half of test_yasc_src_gcc_interop (yasc_src_gcc.c is gcc's). */
extern int printf(const char *, ...);

static const char *base(const char *p)
{
  const char *b = p;
  for (; *p; p++)
    if (*p == '/')
      b = p + 1;
  return b;
}

#pragma yasc
#include <yasc_src.h>

/* the C header of a yasc function lists every argument: gcc passes YASC_SRC */
void gcc_show(const char *what, yasc_src at = @src());
yasc_src gcc_here(void);

int main(void)
{
  gcc_show("tcc-made");
  yasc_src at = gcc_here();
  printf("tcc %s:%u %s\n", base(at.file), at.line, at.func);
  return 0;
}
