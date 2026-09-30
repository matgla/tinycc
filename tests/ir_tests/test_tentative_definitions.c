/* File-scope objects declared without an initializer and defined further
 * down, the way the Zig C backend declares every static: each is laid out
 * once, at the storage its first declaration took (constants) or where its
 * definition puts it (the rest).  Nothing may fold the bytes of a declaration
 * whose definition is still to come, even in a body generated where it stands
 * (a variadic one).  Tentative definitions never completed are zero-filled
 * objects in .bss, static or not, aligned as asked. */
#include <stdarg.h>
#include <stdio.h>

struct pair
{
  int a, b;
};

struct node
{
  const char *name;
  const struct node *next;
  int weight;
};

static const int tbl[3];
static const struct pair pr;
static const struct node second;
static const struct node first;
static const int flag_k;
static int counter;
static int counter;
static int base_a;
static int base_b;
static int base_c;
static int never_set;
int global_never_set;
static int aligned_buf[4] __attribute__((aligned(64)));
static int *const counter_ref;

/* Variadic: generated here, before any definition below. */
int early_read(int n, ...)
{
  va_list ap;
  va_start(ap, n);
  int extra = va_arg(ap, int);
  va_end(ap);
  return tbl[1] * 1000 + pr.b * 100 + flag_k * 10 + n + extra;
}

/* Stores clustered off one base address, before base_a is defined. */
void set_bases(int x)
{
  base_a = x + 3;
  base_b = x + 1;
  base_c = x + 2;
}

static const int tbl[3] = {1, 2, 3};
static const struct pair pr = {7, 8};
static const struct node second = {"second", 0, 20};
static const struct node first = {"first", &second, 10};
static const int flag_k = 5;
static int counter = 40;
static int base_a = 99;
static int *const counter_ref = &counter;

int (*volatile early_p)(int, ...) = early_read;
void (*volatile set_p)(int) = set_bases;

int main(void)
{
  printf("%d\n", early_p(4, 60));
  set_p(10);
  printf("%d %d %d\n", base_a, base_b, base_c);
  int w = 0;
  for (const struct node *n = &first; n; n = n->next)
  {
    printf("%s ", n->name);
    w += n->weight;
  }
  printf("%d\n", w);
  ++*counter_ref;
  printf("%d %d %d\n", counter, never_set, global_never_set);
  printf("%d %d\n", (int)((unsigned long)aligned_buf & 63), aligned_buf[3]);
  return 0;
}
