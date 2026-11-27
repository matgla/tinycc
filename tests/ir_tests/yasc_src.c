/* @src() and yasc default arguments (docs/yasc_default_args.md):
 * @src() as a default is the caller's location, made at each call; other
 * defaults are constants fixed where they are written.  A yasc_src is two
 * words (the function's header, the line); the header's names are
 * self-relative words in .rodata (builtin/misc.c), so they must survive the
 * unit's unreferenced statics being dropped around them at -O1 and up. */
extern int printf(const char *, ...);

/* the file name without its directories (the build passes a full path) */
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

static void where(const char *what, yasc_src at = @src())
{
  printf("%s %s:%u %s [%s]\n", what, base(at.file), at.line, at.func, at.module);
}

/* a prototype gives the defaults, the definition comes later without them */
int scale(int x, int mul = 3, const char *tag = "def");

static int twice(int v) { return 2 * v; }
static int apply(int v, int (*f)(int) = twice) { return f(v); }
static int first(int n = 7, ...) { return n; }
static int deref_or(int *?p = nullptr, int d = -1) { return p ? *p : d; }

/* called once: inlined at -O1, the default still names this function */
static void helper(void) { where("helper"); }

/* kept beyond the call: a yasc_src is a value */
static yasc_src g_last;
static void remember(yasc_src at = @src()) { g_last = at; }

struct counter
{
  int n;
  int bump(inout struct counter *self, int by = 1)
  {
    self->n += by;
    return self->n;
  }
};

static yasc_src pick(int i, yasc_src a = @src(), yasc_src b = @src())
{
  return i ? b : a;
}

static unsigned line_of(const yasc_src *p) { return p->line + (p->file[0] != 0); }

int main(void)
{
  struct counter c = {0};
  int x = 5;

  where("main");
  where("explicit", @src());
  helper();
  printf("scale %d %d %d\n", scale(2), scale(2, 4), scale(2, 4, "x"));
  printf("apply %d first %d %d deref %d\n", apply(7), first(), first(1, 2, 3), deref_or());
  printf("bump %d %d\n", c.bump(), c.bump(10));
  remember();
  printf("remembered %s:%u %s\n", base(g_last.file), g_last.line, g_last.func);
  /* written out: no value made at all */
  printf("line %u\n", @src().line);
  printf("here %s %u\n", @src().func, @src().line);
  /* both defaults of one call: one location */
  printf("pick %u %u\n", pick(0).line, pick(1).line);
  yasc_src copy = @src();
  yasc_src *cp = &copy;
  printf("copy %u %u %s\n", copy.line, line_of(cp), cp->func);
  printf("sizeof %u\n", (unsigned)sizeof(@src()));
  printf("ptr %d\n", deref_or(&x));
  return 0;
}

int scale(int x, int mul, const char *tag)
{
  printf("[%s]", tag);
  return x * mul;
}
