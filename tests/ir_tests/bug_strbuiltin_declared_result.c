/* String builtins take the declared result type (builtin/string.c).
 *
 *  TCC - Tiny C Compiler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * The redirected strchr/strcpy and the folded memchr gave their value the
 * converted argument's type (const char *); now the declaration's.  The
 * values must not change: the type was the only thing wrong.
 */
#include <stdio.h>
#include <string.h>

__attribute__((noinline)) static char *find(char *s, int c) { char *p = strchr(s, c); return p; }
__attribute__((noinline)) static char *rfind(char *s, int c) { char *p = strrchr(s, c); return p; }
__attribute__((noinline)) static char *sub(char *s, const char *n) { char *p = strstr(s, n); return p; }
__attribute__((noinline)) static char *any(char *s, const char *a) { char *p = strpbrk(s, a); return p; }
__attribute__((noinline)) static char *copy(char *d, const char *s) { char *p = strcpy(d, s); return p; }

int main(void)
{
  char buf[16] = "hello, world";
  char dst[16];
  char *p = memchr("abcd", 'c', 4);
  char *q = memchr("abcd", 'z', 4);
  char *f = find(buf, 'o');
  *f = 'O'; /* writable through the result */
  printf("%s\n", buf);
  printf("%d %d %d\n", (int)(find(buf, 'w') - buf), (int)(rfind(buf, 'l') - buf), find(buf, 'z') == 0);
  printf("%d %d\n", (int)(sub(buf, "wor") - buf), (int)(any(buf, ",!") - buf));
  printf("%s\n", copy(dst, "copied"));
  printf("%d\n", copy(dst, "x") == dst);
  printf("%c %d\n", *p, q == 0);
  return 0;
}
