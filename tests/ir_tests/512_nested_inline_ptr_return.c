/* A pointer-returning helper called from inside another inline expansion is
   now inlined too.  It used to be refused below depth 1 as a guard against
   gcc.c-torture 930725-1 (a ternary between two pointer returns); the Zig C
   backend's accessors -- `header()` is `bytes - 4`, `acquire()` a load --
   are exactly these helpers, called from `view()`/`itemPtr()`.  930725-1's
   shape is repeated here one and two expansions deep, beside the accessor
   shape, a pointer into a caller's local, and a pointer that feeds a store. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int v;

static char *g(void) { return ""; }
static char *f(void) { return (v == 0 ? g() : "abc"); }
static inline char *f_deeper(void) { return f(); }
static inline const char *pick(int k) { return k ? f_deeper() : f(); }

struct list { uint8_t *bytes; };
struct header { uint32_t len, cap; };

static struct header *header(struct list const a0)
{
    struct list t0 = a0;
    struct list const *t1 = &t0;
    uint8_t *t3 = t1->bytes;
    t3 = t3 - 8;
    return (struct header *)t3;
}

static uint32_t *items(struct list l) { return (uint32_t *)l.bytes; }

static uint32_t view_sum(struct list l)
{
    struct header *h = header(l);
    uint32_t *it = items(l), s = 0;
    for (uint32_t i = 0; i < h->len; i++)
        s += it[i];
    return s + h->cap;
}

static int *slot(int *base, int i) { return base + (i & 3); }
static int bump(int *base, int i) { int *p = slot(base, i); *p += i; return *p; }

__attribute__((noinline)) static int locals(int x)
{
    int a[4] = {1, 2, 3, 4};
    int r = 0;
    for (int i = 0; i < 6; i++)
        r += bump(a, i + x);
    return r + a[0] + a[1] + a[2] + a[3];
}

int main(void)
{
    v = 1;
    printf("930725 %d %d\n", !strcmp(pick(0), "abc"), !strcmp(pick(1), "abc"));
    v = 0;
    printf("930725 %d %d\n", !strcmp(pick(0), ""), !strcmp(pick(1), ""));

    static uint32_t store[2 + 5] = {5, 99, 10, 20, 30, 40, 50};
    struct list l = { (uint8_t *)&store[2] };
    printf("view %u\n", view_sum(l));
    printf("locals %d %d\n", locals(0), locals(3));
    return 0;
}
