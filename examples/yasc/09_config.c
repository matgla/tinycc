/* Chapter 9 -- putting it together (README.md).
 *
 * 09_settings.txt is built into the program with C23 #embed and parsed into
 * a list that owns its entries.  A bad line is an error value (chapter 5),
 * cleanup is defer and errdefer (chapter 4), the list is a struct with
 * methods (chapter 7), and #pragma tcc diagnostic makes format mistakes in
 * say() calls errors.
 * Build: tcc 09_config.c mem.o -o config */
#pragma yasc
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include "mem.h"

/* The file's bytes, read at compile time; suffix(, 0) ends them with a NUL. */
static const char settings[] = {
#embed "09_settings.txt" suffix(, 0)
};

error ConfigError { NoEquals, EmptyKey, BadNumber, NotFound };

/* The format attribute makes every call of say() checked as printf's are
   (-Wformat): a %d given a string is a warning.  From here on this file
   makes it an error instead, whatever the command line says. */
#pragma tcc diagnostic error "-Wformat"
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

/* s[0..n) as a new string */
static !char * owned copy(const char *s, int n)
{
  char * owned maybe buf = mem_new((size_t)n + 1);
  if (!buf)
    return error.ENOMEM;
  for (int i = 0; i < n; i++)
    buf[i] = s[i];
  buf[n] = 0;
  return buf;
}

static int same(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

struct entry
{
  struct entry * owned maybe next;
  char * owned key;
  char * owned value;
};

struct config
{
  struct entry * owned maybe first;
  int count;

  /* one "key = value" line, s[0..n) */
  !int add(inout struct config *self, const char *s, int n)
  {
    int eq = 0;
    while (eq < n && s[eq] != '=')
      eq++;
    if (eq == n)
      return error.NoEquals;
    int key_end = eq;
    while (key_end > 0 && s[key_end - 1] == ' ')
      key_end--;
    if (key_end == 0)
      return error.EmptyKey;
    int v = eq + 1;
    while (v < n && s[v] == ' ')
      v++;

    char * owned key = try copy(s, key_end);
    errdefer mem_free(key); /* freed if a later step fails */
    char * owned value = try copy(s + v, n - v);
    errdefer mem_free(value);
    struct entry * owned maybe e = mem_new(sizeof *e);
    if (!e)
      return error.ENOMEM; /* key and value are freed */
    e->key = move(key);    /* the entry owns them now */
    e->value = move(value);
    e->next = move(self->first);
    self->first = move(e);
    return ++self->count;
  }

  /* every line of text; a bad one is reported and skipped */
  int parse(inout struct config *self, const char *text)
  {
    int line = 0;
    for (const char * view p = text; *p;)
    {
      int n = 0;
      while (p[n] && p[n] != '\n')
        n++;
      line++;
      if (n > 0 && p[0] != '#')
        self->add(p, n) catch |e| { say("line %d: %s\n", line, error_name(e)); };
      p += n;
      if (*p)
        p++;
    }
    return self->count;
  }

  const char * view maybe text(const struct config *self, const char *key)
  {
    for (const struct entry * view maybe e = self->first; e; e = e->next)
      if (same(e->key, key))
        return e->value;
    return nullptr;
  }

  !int number(const struct config *self, const char *key)
  {
    const char * view maybe s = self->text(key);
    if (!s)
      return error.NotFound;
    int v = 0;
    for (; *s; s++)
    {
      if (*s < '0' || *s > '9')
        return error.BadNumber;
      v = v * 10 + (*s - '0');
    }
    return v;
  }

  void clear(drop struct config *self)
  {
    while (self->first)
    {
      struct entry * owned maybe e = move(self->first);
      self->first = move(e->next);
      mem_free(e->key);
      mem_free(e->value);
      mem_free(e);
    }
    self->count = 0;
  }
};

/* try: a missing or bad number is area's failure */
static !int area(const struct config *cfg)
{
  return try cfg->number("width") * try cfg->number("height");
}

int main(void)
{
  {
    struct config cfg = {};
    defer cfg.clear(); /* the entries are freed when this block ends */

    say("%d settings\n", cfg.parse(settings));
    const char * view maybe name = cfg.text("name");
    say("name: %s\n", name ? name : "(none)");
    say("area: %d\n", area(&cfg) catch -1);
    const char * view keys[] = {"depth", "speed"};
    for (int i = 0; i < 2; i++)
    {
      int v = cfg.number(keys[i]) catch |e| {
        say("%s: %s\n", keys[i], error_name(e));
        continue;
      };
      say("%s: %d\n", keys[i], v);
    }
  }
  say("blocks left: %d\n", mem_live());
  return 0;
}
