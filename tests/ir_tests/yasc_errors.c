/* yasc error unions: `!T`, `error.NAME`, error sets, try, catch, errdefer.
 * docs/plans/yasc_errors.md.  yasc_errors+abi.c is plain C and calls into
 * this file: it sees an error union as an unsigned long long, the value in
 * the low word and the error in the high word. */
#pragma yasc
#include <errno.h>
#include <stdio.h>

error ParseError { BadDigit, Overflow, Empty };
error FsError { FileNotFound = ENOENT, EACCES, BadPath };

void abi_check(void);

static !int parse_digit(char c)
{
  if (c < '0' || c > '9')
    return error.BadDigit;
  return c - '0';
}

static !int parse_u8(const char * view s)
{
  int v = 0;
  if (!*s)
    return error.Empty;
  for (; *s; s++)
  {
    v = v * 10 + try parse_digit(*s);
    if (v > 255)
      return error.Overflow;
  }
  return v;
}

/* called from yasc_errors+abi.c */
!int yasc_parse(const char * view s)
{
  return parse_u8(s);
}

static const char * borrows(static) pname(error e)
{
  if (e == error.BadDigit)
    return "BadDigit";
  if (e == error.Overflow)
    return "Overflow";
  if (e == error.Empty)
    return "Empty";
  if (e == error.ENOENT)
    return "ENOENT";
  return "other";
}

static void t_catch(void)
{
  int a = parse_u8("42") catch 0;
  int b = parse_u8("4x") catch -1;
  int c = parse_u8("999") catch |e| (e == error.Overflow ? 255 : 0);
  printf("catch %d %d %d\n", a, b, c);
  parse_u8("") catch |e| { printf("caught %s\n", pname(e)); };
  int d = parse_u8("7") catch |e| {
    printf("not reached %s\n", pname(e));
    return;
  };
  printf("d %d\n", d);
}

static char buf[4];
static !char * borrows(static) make(int mode)
{
  defer printf("make %d: leave\n", mode);
  errdefer printf("make %d: undo\n", mode);
  if (mode == 1)
    return error.Empty;
  int d = try parse_digit(mode == 2 ? 'x' : '5');
  {
    errdefer printf("make %d: inner undo\n", mode);
    if (mode == 3)
      return error.ENOENT;
  }
  buf[0] = '0' + d;
  buf[1] = 0;
  return buf;
}

static void t_errdefer(void)
{
  for (int m = 0; m < 4; m++)
  {
    char * view s = make(m) catch |e| {
      printf("make %d failed: %s\n", m, pname(e));
      continue;
    };
    printf("make %d -> %s\n", m, s);
  }
}

static int counter;
static !void bump(int by)
{
  if (by < 0)
    return error.Overflow;
  counter += by;
}

static !void bump_twice(int by)
{
  try bump(by);
  try bump(by);
  return;
}

static void t_void(void)
{
  bump_twice(2) catch |e| { printf("unexpected %s\n", pname(e)); };
  bump_twice(-1) catch |e| { printf("bump failed: %s\n", pname(e)); };
  printf("counter %d\n", counter);
}

static !int forward(const char * view s)
{
  return parse_u8(s);
}

static !int with_cleanup(const char * view s)
{
  errdefer printf("forward failed\n");
  defer printf("forward done\n");
  return parse_u8(s);
}

static void t_forward(void)
{
  printf("fwd %d\n", forward("12") catch -1);
  printf("fwd %d\n", with_cleanup("1x") catch -1);
  printf("fwd %d\n", with_cleanup("3") catch -1);
}

static !int open_thing(int which)
{
  error FsError e = error.BadPath;
  if (which == 0)
    e = error.FileNotFound;
  if (which < 2)
    return e;
  return 100 + which;
}

static void t_sets(void)
{
  for (int w = 0; w < 3; w++)
    printf("open %d -> %d\n", w, open_thing(w) catch |e| (e == ENOENT ? -2 : -3));
  error FsError fe = error.EACCES;
  int en = fe;
  printf("fs %d %d %d\n", error.FileNotFound == ENOENT, en == EACCES, error.ENOENT == 2);
  error any = fe;
  printf("any %d\n", any == EACCES);
  printf("hashed %d %d %d %d\n", (int)error.BadDigit, (int)error.Overflow, (int)error.Empty, (int)error.BadPath);
}

static int unwrap_or(!int r, int dflt)
{
  return r catch dflt;
}

static !int from_errno(int code)
{
  if (code)
    return error_from(code);
  return 0;
}

static void t_values(void)
{
  !int r = parse_u8("5");
  !int ok = 6;
  !int bad = error.Empty;
  printf("local %d %d %d %d\n", r catch 0, ok catch 0, bad catch -1, unwrap_or(parse_u8("x"), 9));
  printf("from %d %d\n", from_errno(EACCES) catch |e| e, from_errno(0) catch -1);
  !int (*fp)(const char * view) = parse_u8;
  printf("fp %d\n", fp("200") catch -1);
}

int main(void)
{
  t_catch();
  t_errdefer();
  t_void();
  t_forward();
  t_sets();
  t_values();
  abi_check();
  return 0;
}
