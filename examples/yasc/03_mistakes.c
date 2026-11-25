/* Chapter 3 -- what the checks catch (README.md).  This file is MEANT to warn:
 *
 *   tcc -O1 -c 03_mistakes.c    (safe by default: the checks are on; -O1 for
 *                               the last one, which the optimizer finds)
 *
 * Each function below has one mistake; 03_mistakes.diag lists what tcc says
 * about it (the test suite compares the two). */
#pragma yasc
#include "mem.h"

void leak(void)
{
  char * owned maybe s = mem_strdup("lost");
  if (!s)
    return;
} /* forgot mem_free(s) */

void double_free(void)
{
  char * owned maybe s = mem_strdup("x");
  mem_free(s);
  mem_free(s);
}

int use_after_free(void)
{
  char * owned maybe s = mem_strdup("x");
  mem_free(s);
  return s != nullptr && s[0] == 'x';
}

int null_deref(void)
{
  char * owned maybe s = mem_strdup("x");
  int c = s[0]; /* no null test */
  mem_free(s);
  return c;
}

void discarded(void)
{
  mem_strdup("x"); /* an owned result nobody frees */
}

void not_mine(inout char * view borrowed)
{
  mem_free(borrowed); /* a view owns nothing to free */
}

int read_out(out int * view n)
{
  return *n; /* out: written by this function, not read */
}

int uninitialized(int c)
{
  int x;
  if (c)
    x = 1;
  return x;
}

int * borrows(static) local_address(void)
{
  int x = 1;
  return &x;
}

void null_argument(void)
{
  char * owned maybe s = mem_strdup(nullptr); /* nonnull(1) */
  mem_free(s);
}

/* The checks below came later: a maybe member, a borrow, a frame address
 * that escapes, and a null the optimizer finds. */

struct node
{
  struct node * owned maybe next; /* the rest of the list, or null */
  int v;
};

int maybe_member(const struct node * view q)
{
  return q->next->v; /* q->next may be null */
}

static int length(const char * view s) /* s may not be null */
{
  int n = 0;
  while (s[n])
    n++;
  return n;
}

int maybe_argument(const char * view maybe s)
{
  return length(s); /* test s first */
}

int borrow_outlives_owner(void)
{
  char * owned maybe s = mem_strdup("abc");
  if (!s)
    return 0;
  char * view tail = s + 1; /* borrows from s */
  mem_free(s);
  return tail[0]; /* s is gone, and tail with it */
}

void dangling(inout int * view * view slot)
{
  int x = 1;
  *slot = &x; /* x dies when dangling returns; *slot lives on */
}

static int * borrows(static) maybe nothing(void)
{
  return nullptr;
}

int null_dereference(void)
{
  return *nothing(); /* after inlining: a load from address 0 */
}

/* Arrays and handles (chapter 10): a loop over an owned array is followed
 * element by element, and an int handle is owned like a pointer. */
#include "slots.h"

int array_leak(void)
{
  char * owned maybe a[4] = undefined;
  int i, n = 0;
  for (i = 0; i < 4; i++)
    a[i] = mem_strdup("x");
  for (i = 0; i < 4; i++)
    n += a[i] != nullptr;
  return n; /* no loop freed them */
}

void array_freed_twice(void)
{
  char * owned maybe a[4] = undefined;
  int i;
  for (i = 0; i < 4; i++)
    a[i] = mem_strdup("x");
  for (i = 0; i < 4; i++)
    mem_free(a[i]);
  for (i = 0; i < 4; i++)
    mem_free(a[i]);
}

int handle_leak(void)
{
  int owned(slot_close) maybe h = slot_open(1);
  if (h < 0)
    return -1;
  return slot_read(h); /* forgot slot_close(h) */
}

int handle_untested(void)
{
  int owned(slot_close) maybe h = slot_open(1);
  int v = slot_read(h); /* h may be -1 */
  slot_close(h);
  return v;
}
