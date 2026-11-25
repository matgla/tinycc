/* Chapter 10 -- owned arrays and handles (README.md).
 *
 * An array can own what its elements point to, and a loop over it is
 * followed element by element: allocate in one loop, free in another, and
 * the checks know every element was freed.  Handles (ints that own a
 * resource, like file descriptors) follow the same rules as pointers.
 * Build: tcc -ccompat -c mem.c slots.c && tcc 10_arrays.c mem.o slots.o */
#pragma yasc
#include <stdio.h>
#include "mem.h"
#include "slots.h"

#define N 4

static const char * view const words[N] = {"alpha", "beta", "gamma", "delta"};

/* an array of owned strings: one loop fills it, one frees it */
static int total_length(void)
{
  char * owned maybe copy[N] = undefined; /* the first loop fills it */
  int i, total = 0;
  for (i = 0; i < N; i++)
    copy[i] = mem_strdup(words[i]);
  for (i = 0; i < N; i++)
    if (copy[i]) /* each element is maybe: test it before use */
      for (int k = 0; copy[i][k]; k++)
        total++;
  for (i = 0; i < N; i++)
    mem_free(copy[i]); /* every element, so nothing leaks */
  return total;
}

/* an array of handles: the same, with slot_close for mem_free */
static int read_all(void)
{
  int owned(slot_close) maybe h[N] = undefined;
  int i, sum = 0;
  for (i = 0; i < N; i++)
    h[i] = slot_open(i);
  for (i = 0; i < N; i++)
    if (h[i] >= 0) /* a handle is valid when >= 0 */
      sum += slot_read(h[i]);
  printf("open while reading: %d\n", slot_live());
  for (i = 0; i < N; i++)
    slot_close(h[i]); /* slot_close takes an invalid handle too */
  return sum;
}

/* one handle, with every way out closing it */
static int first_value(int which)
{
  int owned(slot_close) maybe h = slot_open(which);
  if (h < 0)
    return -1; /* nothing was opened: nothing to close */
  defer slot_close(h);
  return slot_read(h);
}

/* a struct array whose elements own a buffer each */
struct line
{
  char * owned maybe text;
  int n;
};

static int count_lines(void)
{
  struct line lines[N] = undefined;
  int i, kept = 0;
  for (i = 0; i < N; i++)
  {
    lines[i].text = mem_strdup(words[i]);
    lines[i].n = i;
  }
  for (i = 0; i < N; i++)
    kept += lines[i].text != nullptr;
  for (i = 0; i < N; i++)
    mem_free(lines[i].text);
  return kept;
}

int main(void)
{
  printf("total length: %d\n", total_length());
  printf("sum: %d\n", read_all());
  printf("first value: %d\n", first_value(7));
  printf("lines: %d\n", count_lines());
  printf("blocks left: %d, handles left: %d\n", mem_live(), slot_live());
  return 0;
}
