/* counter.c -- regular C (build it with -ccompat): the library of counter.h. */
#include <stdlib.h>
#include <string.h>
#include "counter.h"

struct counter
{
  int value;
  char name[16];
};

counter_t *counter_new(const char *name)
{
  counter_t *c = malloc(sizeof *c);
  if (c)
  {
    c->value = 0;
    strncpy(c->name, name, sizeof c->name - 1);
    c->name[sizeof c->name - 1] = 0;
  }
  return c;
}

void counter_add(counter_t *c, int n)
{
  c->value += n;
}

int counter_get(const counter_t *c)
{
  return c->value;
}

const char *counter_name(const counter_t *c)
{
  return c->name;
}

void counter_free(counter_t *c)
{
  free(c);
}
