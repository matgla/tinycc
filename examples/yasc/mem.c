/* mem.c -- regular C (build it with -ccompat): the allocator of mem.h. */
#include <stdlib.h>
#include <string.h>
#include "mem.h"

static int live;

void *mem_new(size_t n)
{
  void *p = malloc(n);
  if (p)
    live++;
  return p;
}

char *mem_strdup(const char *s)
{
  size_t n = strlen(s) + 1;
  char *d = mem_new(n);
  if (d)
    memcpy(d, s, n);
  return d;
}

void mem_free(void *p)
{
  if (p)
    live--;
  free(p);
}

int mem_live(void)
{
  return live;
}
