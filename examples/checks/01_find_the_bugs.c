/* Chapter 1 -- find the bugs (README.md).  This file is MEANT to warn:
 *
 *   tcc -ccompat -Wall -Wextra -Wuninitialized -Wmaybe-uninitialized \
 *       -Wdangling-pointer -Wnull-dereference -Wformat-security -c 01_find_the_bugs.c
 *
 * It is plain C (-ccompat): the checks below need no yasc.  Each function
 * has one bug; 01_find_the_bugs.diag is what tcc says about them, the same
 * at every -O (the test suite compares the two).  01_fixed.c is the same
 * program with every bug fixed: it compiles clean with -Werror, and runs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum color { RED, GREEN, BLUE };

/* the format attribute: tcc checks say()'s calls as it checks printf's */
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* -Wformat: the conversion does not match the argument */
void print_count(long n)
{
  say("%d items\n", n);
}

/* -Wformat-security: a format nobody wrote */
void print_name(const char *name)
{
  say(name);
}

/* -Wparentheses: = where == was meant */
int is_empty(int len)
{
  if (len = 0)
    return 1;
  return 0;
}

/* -Wimplicit-fallthrough: a missing break */
int weight(int c)
{
  int w = 0;
  switch (c)
  {
  case 'a':
    w += 1;
  case 'b':
    w += 2;
    break;
  default:
    break;
  }
  return w;
}

/* -Wswitch: BLUE is not handled */
const char *color_name(enum color c)
{
  switch (c)
  {
  case RED:
    return "red";
  case GREEN:
    return "green";
  }
  return "?";
}

/* -Wsign-compare (-Wextra): -1 < 4u is false */
int below(int i, unsigned n)
{
  return i < n;
}

/* -Wtype-limits (-Wextra): an unsigned value is never negative */
int negative(unsigned v)
{
  return v < 0;
}

/* -Wshift-count-overflow */
unsigned high_bit(void)
{
  return 1u << 32;
}

/* -Wunused-variable */
int sum3(int a, int b, int c)
{
  int total;
  return a + b + c;
}

/* -Warray-bounds: a constant index past the end */
int last(void)
{
  int a[4] = {1, 2, 3, 4};
  return a[4];
}

/* -Warray-bounds: the loop's last trip is past the end */
int total(void)
{
  int a[4] = {1, 2, 3, 4};
  int s = 0;
  for (int i = 0; i <= 4; i++)
    s += a[i];
  return s;
}

/* -Wsizeof-pointer-memaccess: sizeof the pointer, not the buffer */
void clear(char *p)
{
  memset(p, 0, sizeof p);
}

/* -Wstringop-overflow: 14 bytes into 8 */
void copy_name(void)
{
  char buf[8];
  strcpy(buf, "a long string");
  puts(buf);
}

/* -Wformat-overflow: "item-" and a number do not fit in 6 bytes */
void label(int n)
{
  char buf[6];
  sprintf(buf, "item-%d", n);
  puts(buf);
}

/* -Wmaybe-uninitialized: x is set on one path only */
int pick(int c)
{
  int x;
  if (c)
    x = 1;
  return x;
}

/* -Wreturn-local-addr: buf dies with the function */
char *make_greeting(void)
{
  char buf[16];
  strcpy(buf, "hello");
  return buf;
}

/* -Wdangling-pointer: x dies, *slot lives on */
void remember(int **slot)
{
  int x = 1;
  *slot = &x;
}

/* -Wfree-nonheap-object: n is not on the heap */
void release(void)
{
  int n = 0;
  free(&n);
}

/* -Wtautological-compare: always true */
int same(int x)
{
  return x == x;
}

/* -Wnull-dereference: p is null where it is read */
int first(void)
{
  int *p = NULL;
  return *p;
}
