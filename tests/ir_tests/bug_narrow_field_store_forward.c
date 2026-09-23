/* A narrow struct field's store truncates; forwarding it must too.
 *
 * sl_forward replaces a load by the value a matching store wrote.  For a
 * field of a struct the frontend does NOT truncate ahead of that store --
 * `s.a = x` stores x whole and lets the store's width cut it -- so handing the
 * register on unmasked passes the bits the store dropped.  From -O1 up
 * `s.a = x + i; use(s.a)` read x + i instead of (x + i) & 0xff.  (A narrow
 * LOCAL is safe: the frontend truncates before writing one, which is why this
 * only ever showed on fields.)
 *
 * Both widths, and a second field beside the narrow one so the object is a
 * real struct rather than a one-field wrapper. */
#include <stdio.h>

struct b1
{
  unsigned char a;
  unsigned char b;
};

struct b2
{
  unsigned short a;
  unsigned w;
};

__attribute__((noinline)) static unsigned byte_field(unsigned x, int n)
{
  struct b1 s;
  unsigned r = 0;
  s.a = 0;
  s.b = 7;
  for (int i = 0; i < n; i++)
  {
    s.a = x + (unsigned)i; /* wraps past 255 */
    r += s.a + s.b;
  }
  return r;
}

__attribute__((noinline)) static unsigned half_field(unsigned x, int n)
{
  struct b2 s;
  unsigned r = 0;
  s.a = x * 3u;
  s.w = x;
  for (int i = 0; i < n; i++)
  {
    s.a = (unsigned)i * 70000u; /* wraps past 65535 */
    r += s.a + s.w;
  }
  return r;
}

int main(void)
{
  printf("%u %u\n", byte_field(250, 10), byte_field(3, 1));
  printf("%u %u\n", half_field(30000, 5), half_field(1, 2));
  return 0;
}
