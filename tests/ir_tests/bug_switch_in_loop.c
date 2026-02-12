/* Bug: switch inside a while loop with complex code before the switch.
 *
 * TCC ARM codegen emits the switch expression assignment (c = *s++)
 * but fails to emit the switch dispatch and case bodies when:
 *   - The switch is inside a while loop
 *   - There is substantial code before the switch in the loop body
 *   - The switch cases are sparse char literals
 *
 * This is a simplified reproduction of vfprintf where %s/%d/etc.
 * format specifiers produce no output.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Minimal FILE-like output to a buffer */
static char outbuf[256];
static int outpos;

static void out_char(int c)
{
  if (outpos < (int)sizeof(outbuf) - 1)
    outbuf[outpos++] = c;
}

static void out_str(const char *s)
{
  while (*s)
    out_char(*s++);
}

static void out_int(long v)
{
  char tmp[32];
  int i = 0;
  int neg = 0;
  unsigned long uv;
  if (v < 0)
  {
    neg = 1;
    uv = -v;
  }
  else
  {
    uv = v;
  }
  do
  {
    tmp[i++] = '0' + (uv % 10);
    uv /= 10;
  } while (uv);
  if (neg)
    tmp[i++] = '-';
  while (i > 0)
    out_char(tmp[--i]);
}

/* Mimics the structure of vfprintf: a format-string parser with a sparse
 * char switch inside a while loop, preceded by non-trivial parsing code. */
static int my_format(const char *fmt, long a0, long a1, long a2, long a3)
{
  const char *s = fmt;
  int n = 0;
  int argidx = 0;
  long args[4];
  args[0] = a0;
  args[1] = a1;
  args[2] = a2;
  args[3] = a3;

  while (*s)
  {
    int c = (unsigned char)*s++;
    int wid = 0;
    int flags = 0;
    char *f;

    if (c != '%')
    {
      out_char(c);
      ++n;
      continue;
    }

    /* Parse flags (mirrors vfprintf) */
    while (*s == '-' || *s == '+' || *s == ' ' || *s == '#' || *s == '0')
    {
      if (*s == '-')
        flags |= 1;
      if (*s == '0')
        flags |= 16;
      s++;
    }

    /* Parse width */
    while (*s >= '0' && *s <= '9')
    {
      wid *= 10;
      wid += *s++ - '0';
    }

    /* Parse precision */
    int max_len = INT_MAX;
    if (*s == '.')
    {
      ++s;
      max_len = 0;
      while (*s >= '0' && *s <= '9')
      {
        max_len *= 10;
        max_len += *s++ - '0';
      }
    }

    /* Parse length modifiers */
    while (*s == 'l' || *s == 'h')
      s++;

    /* The critical switch - sparse char cases */
    switch ((c = *s++))
    {
    case 'd':
    case 'i':
      if (argidx < 4)
        out_int(args[argidx++]);
      ++n;
      break;
    case 'u':
      if (argidx < 4)
        out_int(args[argidx++]);
      ++n;
      break;
    case 'o':
      out_char('O');
      if (argidx < 4)
        argidx++;
      ++n;
      break;
    case 'p':
      flags |= 8;
      /* fallthrough */
    case 'x':
      out_char('X');
      if (argidx < 4)
        argidx++;
      ++n;
      break;
    case 'X':
      out_char('H');
      if (argidx < 4)
        argidx++;
      ++n;
      break;
    case 'c':
      if (argidx < 4)
        out_char((char)args[argidx++]);
      ++n;
      break;
    case 's':
      if (argidx < 4)
        out_str((const char *)args[argidx++]);
      ++n;
      break;
    case 'n':
      ++n;
      break;
    case '\0':
      s--;
      break;
    default:
      out_char(c);
      ++n;
      break;
    }
  }
  return n;
}

int main(void)
{
  outpos = 0;
  my_format("hello %s, num=%d!\n", (long)"world", 42, 0, 0);
  outbuf[outpos] = '\0';
  printf("result: [%s]\n", outbuf);

  outpos = 0;
  my_format("%c%c%c\n", (long)'A', (long)'B', (long)'C', 0);
  outbuf[outpos] = '\0';
  printf("result: [%s]\n", outbuf);

  outpos = 0;
  my_format("%d+%d=%d\n", 10, 20, 30, 0);
  outbuf[outpos] = '\0';
  printf("result: [%s]\n", outbuf);

  return 0;
}
