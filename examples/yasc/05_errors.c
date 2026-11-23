/* Chapter 5 -- errors as values: error sets, !T, try, catch, errdefer
 * (README.md).
 *
 * `!T` is "a T or an error".  `return error.NAME;` fails, `try f()` passes a
 * failure up, `f() catch X` handles it.  errno names are errors too
 * (error.ENOMEM), and error_name() spells any of them.
 * Build: tcc 05_errors.c mem.o -o errors */
#pragma yasc
#include <errno.h>
#include <stdio.h>
#include "mem.h"

error ParseError { Empty, BadDigit, TooBig };

static !int parse_int(const char * view s)
{
  int v = 0;
  if (!*s)
    return error.Empty;
  for (; *s; s++)
  {
    if (*s < '0' || *s > '9')
      return error.BadDigit;
    v = v * 10 + (*s - '0');
    if (v > 9999)
      return error.TooBig;
  }
  return v;
}

/* try: a failure of either parse is this function's failure */
static !int add(const char * view a, const char * view b)
{
  return try parse_int(a) + try parse_int(b);
}

/* errdefer runs only when the function fails: the buffer is freed on the
   error paths, and handed to the caller on success. */
static !char * owned make_label(const char * view num)
{
  int v = try parse_int(num);
  char * owned maybe buf = mem_new(32);
  if (!buf)
    return error.ENOMEM;
  errdefer mem_free(buf);
  if (v == 0)
    return error.Empty;
  snprintf(buf, 32, "item-%d", v);
  return buf;
}

int main(void)
{
  printf("%d %d\n", parse_int("42") catch -1, parse_int("4x") catch -1);

  int sum = add("12", "30") catch |e| {
    printf("add failed: %s\n", error_name(e));
    return 1;
  };
  printf("sum %d\n", sum);
  add("1", "") catch |e| { printf("add failed: %s\n", error_name(e)); };

  const char * view inputs[] = {"7", "0", "x", "123456"};
  for (int i = 0; i < 4; i++)
  {
    char * owned label = make_label(inputs[i]) catch |e| {
      printf("label %s: %s\n", inputs[i], error_name(e));
      continue;
    };
    printf("label %s: %s\n", inputs[i], label);
    mem_free(label);
  }
  printf("blocks left: %d\n", mem_live());
  return 0;
}
