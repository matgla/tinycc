/* Chapter 1 -- the bugs of 01_find_the_bugs.c, fixed (README.md).
 *
 *   tcc -ccompat -Wall -Wextra -Wuninitialized -Wmaybe-uninitialized \
 *       -Wdangling-pointer -Wnull-dereference -Wformat-security -Werror \
 *       01_fixed.c -o fixed
 *
 * Each function says what changed.  The program prints 01_fixed.expect. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum color { RED, GREEN, BLUE };

/* the format attribute: tcc checks say()'s calls as it checks printf's */
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void say(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

/* %ld for a long */
void print_count(long n)
{
  say("%ld items\n", n);
}

/* the name is an argument, not the format */
void print_name(const char *name)
{
  say("%s\n", name);
}

/* == compares */
int is_empty(int len)
{
  if (len == 0)
    return 1;
  return 0;
}

/* the fall through is meant, and says so */
int weight(int c)
{
  int w = 0;
  switch (c)
  {
  case 'a':
    w += 1;
    /* fall through */
  case 'b':
    w += 2;
    break;
  default:
    break;
  }
  return w;
}

/* every enumerator has its case */
const char *color_name(enum color c)
{
  switch (c)
  {
  case RED:
    return "red";
  case GREEN:
    return "green";
  case BLUE:
    return "blue";
  }
  return "?";
}

/* a negative i is below any n */
int below(int i, unsigned n)
{
  return i < 0 || (unsigned)i < n;
}

/* the test that was meant: the top bit */
int negative(unsigned v)
{
  return (v >> 31) != 0;
}

/* the top bit of 32 */
unsigned high_bit(void)
{
  return 1u << 31;
}

/* no variable left over */
int sum3(int a, int b, int c)
{
  return a + b + c;
}

/* the last element is a[3] */
int last(void)
{
  int a[4] = {1, 2, 3, 4};
  return a[3];
}

/* < 4, not <= 4 */
int total(void)
{
  int a[4] = {1, 2, 3, 4};
  int s = 0;
  for (int i = 0; i < 4; i++)
    s += a[i];
  return s;
}

/* the caller says how long the buffer is */
void clear(char *p, size_t n)
{
  memset(p, 0, n);
}

/* a buffer that fits, and a bounded copy */
void copy_name(void)
{
  char buf[16];
  snprintf(buf, sizeof buf, "%s", "a long string");
  puts(buf);
}

/* snprintf cuts the label at the buffer's size */
void label(int n)
{
  char buf[12];
  snprintf(buf, sizeof buf, "item-%d", n);
  puts(buf);
}

/* x has a value on every path */
int pick(int c)
{
  int x = 0;
  if (c)
    x = 1;
  return x;
}

/* the caller's buffer outlives the call */
char *make_greeting(char *buf, size_t n)
{
  snprintf(buf, n, "hello");
  return buf;
}

/* a static outlives the call */
void remember(int **slot)
{
  static int x = 1;
  *slot = &x;
}

/* only what malloc gave goes to free */
void release(void)
{
  int *n = malloc(sizeof *n);
  free(n);
}

/* compare with something else */
int same(int x, int y)
{
  return x == y;
}

/* test before reading */
int first(const int *p)
{
  return p ? *p : -1;
}

int main(void)
{
  char text[8] = "abc";
  char greeting[16];
  int *kept;
  int seven = 7;

  print_count(3);
  print_name("tcc");
  printf("empty: %d %d\n", is_empty(0), is_empty(5));
  printf("weight: %d %d\n", weight('a'), weight('b'));
  printf("colors: %s %s %s\n", color_name(RED), color_name(GREEN), color_name(BLUE));
  printf("below: %d %d\n", below(-1, 4), below(5, 4));
  printf("negative: %d %d\n", negative(0x80000000u), negative(1));
  printf("high bit: %08x\n", high_bit());
  printf("sum3: %d, last: %d, total: %d\n", sum3(1, 2, 3), last(), total());
  clear(text, sizeof text);
  printf("cleared: %d\n", text[0]);
  copy_name();
  label(42);
  printf("pick: %d %d\n", pick(0), pick(1));
  printf("%s\n", make_greeting(greeting, sizeof greeting));
  remember(&kept);
  printf("kept: %d\n", *kept);
  release();
  printf("same: %d %d\n", same(1, 1), same(1, 2));
  printf("first: %d %d\n", first(NULL), first(&seven));
  return 0;
}
