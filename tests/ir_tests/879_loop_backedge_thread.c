/* Post-regalloc jump threading of a conditional branch onto a loop head
 * (ra:jt_backward).  Each `continue` below reaches the head through a
 * one-instruction `b head` trampoline in the un-threaded code; threading aims
 * the conditional branches straight at the head, and the last test of the
 * chain is inverted over the trampoline.  Results must not change:
 *   ident_len   -- `||` chain of ranges, every true arm continues
 *   skip_ws     -- a state machine: switch inside the loop, continue per case
 *   count_until -- the continue path also updates a register-carried value
 *   nested      -- an inner continue-loop inside an outer loop
 * plus boundary characters on both sides of every range. */
#include <stdio.h>

struct T { const unsigned char *buf; int idx; };

static int ident_len(struct T *t)
{
  int start = t->idx;
  for (;;)
  {
    t->idx++;
    unsigned char c = t->buf[t->idx];
    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
      continue;
    return t->idx - start;
  }
}

static int skip_ws(const char *s)
{
  int state = 0, i = 0, n = 0;
  for (;;)
  {
    char c = s[i++];
    switch (state)
    {
    case 0:
      if (c == ' ' || c == '\t')
        continue;
      if (c == '/')
      {
        state = 1;
        continue;
      }
      return n * 1000 + i;
    case 1:
      if (c == '/')
      {
        state = 2;
        n++;
        continue;
      }
      return -i;
    default:
      if (c == '\n')
      {
        state = 0;
        continue;
      }
      if (c == 0)
        return n * 1000 + i + 500;
      continue;
    }
  }
}

static unsigned count_until(const unsigned char *p, unsigned lim)
{
  unsigned acc = 7, i = 0;
  for (;;)
  {
    unsigned char c = p[i++];
    if (c == 0 || i > lim)
      return acc * 31 + i;
    if (c & 1)
    {
      acc += c;
      continue;
    }
    if (c > 200)
      continue;
    acc ^= c << 3;
  }
}

static unsigned nested(const char *const *v, int n)
{
  unsigned total = 0;
  for (int k = 0; k < n; k++)
  {
    struct T t = {(const unsigned char *)v[k], -1};
    total = total * 7u + (unsigned)ident_len(&t);
  }
  return total;
}

int main(void)
{
  static const char *ids[] = {"", "_", "a", "z", "A", "Z", "0", "9", "`x", "{", "@", "[", "/", ":", "ab_9Z ", "x-y",
                              "__a0Zz09azAZ$"};
  for (unsigned k = 0; k < sizeof(ids) / sizeof(ids[0]); k++)
  {
    struct T t = {(const unsigned char *)ids[k], -1};
    printf("ident[%u]=%d\n", k, ident_len(&t));
  }
  printf("nested=%u\n", nested(ids, (int)(sizeof(ids) / sizeof(ids[0]))));
  static const char *ws[] = {"x", "  x", "\t\t \ty", "// c\n  z", "/x", "//\n//\n// e", "/", "  // a\n\t// b"};
  for (unsigned k = 0; k < sizeof(ws) / sizeof(ws[0]); k++)
    printf("ws[%u]=%d\n", k, skip_ws(ws[k]));
  static const unsigned char bytes[] = {1, 2, 3, 201, 202, 255, 254, 200, 199, 16, 0, 9};
  for (unsigned lim = 0; lim < 14; lim += 3)
    printf("count(%u)=%u\n", lim, count_until(bytes, lim));
  return 0;
}
