/* _FORTIFY_SOURCE cases (test_fortify.py): argv[1] names one.  The sizes
 * come from volatiles, so the compiler cannot decide the check: the
 * __*_chk function runs and either lets the call through ("ok <case>") or
 * ends the program with "*** buffer overflow detected ***". */
#include "fortify_libc.h"

volatile int v7 = 7, v8 = 8, v9 = 9, v12 = 12;
static const char *const long8 = "12345678", *const long7 = "1234567";

struct S
{
  char a[4];
  int x;
};

char gbuf[8];

/* the destination a global: its size is known inside the helper */
static int gvs(const char *fmt, ...)
{
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsprintf(gbuf, fmt, ap);
  va_end(ap);
  return r;
}

static int gvsn(int n, const char *fmt, ...)
{
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(gbuf, n, fmt, ap);
  va_end(ap);
  return r;
}

__attribute__((noinline)) static int run(const char *c)
{
  char b[8];
  const char *s = !strcmp(c + strlen(c) - 3, "_ok") ? long7 : long8;
  int n = !strcmp(c + strlen(c) - 3, "_ok") ? v8 : v9;
  memset(b, 0, sizeof b);
  if (!strncmp(c, "memcpy", 6))
    memcpy(b, "abcdefghij", n);
  else if (!strncmp(c, "memmove", 7))
    memmove(b, "abcdefghij", n);
  else if (!strncmp(c, "memset", 6))
    memset(b, 'x', n);
  else if (!strncmp(c, "strcpy", 6))
    strcpy(b, s);
  else if (!strncmp(c, "stpcpy", 6))
    stpcpy(b, s);
  else if (!strncmp(c, "strncpy", 7))
    strncpy(b, "ab", n);
  else if (!strncmp(c, "strcat", 6))
  {
    b[0] = 'x';
    strcat(b, s + 1);
  }
  else if (!strncmp(c, "strncat", 7))
    strncat(b, "abcdefghij", n - 1);
  else if (!strncmp(c, "sprintf", 7))
    sprintf(b, "%s", s);
  else if (!strncmp(c, "snprintf", 8))
    snprintf(b, n, "%s", "x");
  else if (!strncmp(c, "vsprintf", 8))
    gvs("%s", s);
  else if (!strncmp(c, "vsnprintf", 9))
    gvsn(n, "%s", "x");
  else if (!strncmp(c, "heap", 4))
  {
    char *p = malloc(8);
    memcpy(p, "abcdefghij", n);
    free(p);
  }
  else if (!strncmp(c, "member", 6))
  {
    /* level 2 checks the member (4 bytes), level 1 the whole struct (8) */
    struct S st;
    strcpy(st.a, s + 4);
    b[0] = st.a[0];
  }
  else if (!strncmp(c, "dynamic", 7))
  {
    /* malloc(n): level 3 knows its size at run time; 1 and 2 nothing */
    int m = v12;
    char *p = malloc(m);
    memcpy(p, "abcdefghijklmnop", m + n - 8);
    free(p);
  }
  else
    return -1;
  return b[0];
}

int main(int argc, char **argv)
{
  const char *c = argc > 1 ? argv[1] : "none";
  int r;
  printf("go %s\n", c);
  r = run(c);
  printf("ok %s %d\n", c, r);
  return 0;
}
