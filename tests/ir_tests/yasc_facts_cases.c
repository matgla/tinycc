/* -fyasc-facts (S4): yasc code whose frame objects are handed to `view`
 * parameters.  Compiled in the safe default by test_yasc_facts.py, with and
 * without the facts, and linked with yasc_facts_main.c (regular C), which
 * defines the callees and prints the results: both builds must print the
 * same. */
struct P
{
  int a, b, c, d;
};

int use(const struct P * view p);   /* reads */
void fill(inout struct P * view p, int v); /* writes every member */
void bump(inout struct P * view p);       /* writes one member */
int say(int x);                     /* touches no P */

/* read-only parameter, then another call: t's stores survive */
int case_read_then_call(int x)
{
  struct P t = {x, x + 1, x + 2, x + 3};
  int r = use(&t);
  r += say(r);
  return r + t.a + t.d;
}

/* a parameter that writes: the stores before it are gone after it */
int case_written(int x)
{
  struct P t = {x, x + 1, x + 2, x + 3};
  fill(&t, 7);
  int r = say(1);
  return r + t.a + t.d;
}

/* one member written, the others kept */
int case_partly_written(int x)
{
  struct P t = {x, x + 1, x + 2, x + 3};
  bump(&t);
  int r = say(2);
  return r + t.a + t.b + t.c + t.d;
}

/* written in a loop, read between calls */
int case_loop(int n)
{
  struct P t = {0, 0, 0, 0};
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    t.a = i;
    s += say(t.a);
    bump(&t);
    s += t.b + t.a;
  }
  return s;
}

/* two objects, one handed on, the other not */
int case_two(int x)
{
  struct P t = {x, x, x, x};
  struct P u = {1, 2, 3, 4};
  fill(&t, x + 5);
  int r = use(&u) + say(3);
  return r + t.c + u.c;
}

/* the address kept in a temp and offset before it is handed on */
int case_offset(int x)
{
  int arr[8] = {0};
  arr[2] = x;
  int r = use((const struct P * view)(arr + 1)) + say(4);
  return r + arr[2] + arr[1];
}

/* ---- O4: an exclusive parameter (restrict view, owned) ---- */
struct B
{
  int len, cap, data[8];
};

/* a field reloaded after a store through another pointer */
int case_restrict_reload(struct B * restrict view b, inout int * view other, int n)
{
  int x = b->cap;
  other[2] = n;
  int y = b->cap;
  return x * 3 + y * 5;
}

/* an element reloaded after a store through another pointer */
int case_restrict_array(int * restrict view d, inout int * view s, int n)
{
  int x = d[3];
  s[1] = n;
  return x * 7 + d[3] * 11;
}

/* a store through the restrict pointer itself is seen */
int case_restrict_self(inout int * restrict view d, int n)
{
  int x = d[3];
  d[3] = n;
  return x + d[3];
}

/* through a pointer derived from it */
int case_restrict_derived(inout int * restrict view d, inout int * view s, int n)
{
  int x = d[3];
  int * view e = d + 2;
  e[1] = n + 1;
  s[0] = n;
  return x + d[3];
}

/* through a pointer derived at a run-time offset: no common base, the
   derivation alone says it may be the same memory */
int case_restrict_runtime_offset(inout int * restrict view d, int k, int n)
{
  int x = d[3];
  int * view e = d + k;
  e[0] = n;
  return x * 100 + d[3];
}

/* ---- O2: a fresh owned result is new memory ---- */
void * owned maybe malloc(unsigned n);
void free(drop void * owned maybe p);
int peek(const int * view p);

/* reloaded after a store through another pointer, then freed */
int case_fresh(inout int * view other, int n)
{
  int * owned maybe p = malloc(32);
  if (!p)
    return -1;
  p[3] = n;
  int x = p[3];
  other[1] = n + 1;
  int y = p[3];
  free(p);
  return x * 3 + y;
}

/* an exclusive parameter handed to a parameter that does not escape it */
int case_restrict_peeked(int * restrict view d, inout int * view s, int n)
{
  int x = d[4];
  int z = peek(d);
  s[2] = n;
  return x * 1000 + z * 10 + d[4];
}

/* ---- O1 for scalars: `int x; f(&x);` with a no-escape parameter ---- */
int peeki(const int * view p);  /* reads */
void puti(inout int * view p, int v); /* writes */
void bumpi(inout int * view p);       /* reads and writes */
long long peekll(const long long * view p);

/* read by the call, kept across another one */
int case_scalar_read(int n)
{
  int x = n + 1;
  int r = peeki(&x);
  r += say(r);
  return r + x;
}

/* an out parameter: nothing to store before, reloaded after */
int case_scalar_out(int n)
{
  int x = undefined;
  puti(&x, n * 2);
  int r = say(5);
  return r + x;
}

/* changed directly and by the call, in a loop */
int case_scalar_loop(int n)
{
  int x = 1, s = 0;
  for (int i = 0; i < n; i++)
  {
    x += i;
    bumpi(&x);
    s += say(x) + x;
  }
  return s + x;
}

/* set on one path only before the call */
int case_scalar_path(int n)
{
  int x = 0;
  if (n > 3)
    x = n;
  bumpi(&x);
  return x * 10 + say(6);
}

/* written directly between two read-only calls */
int case_scalar_twice(int n)
{
  int x = n;
  int a = peeki(&x);
  x = x * 3;
  int b = peeki(&x);
  return a * 100 + b + x;
}

/* a 64-bit scalar */
int case_scalar_ll(int n)
{
  long long v = (long long)n << 33;
  long long r = peekll(&v);
  r += say(7);
  return (int)(r >> 20) + (int)(v >> 32);
}

/* ---- O1 in dead_local_slot: stores no later call can read ---- */

/* after the last call that reads t: dead */
int case_dead_after(int x)
{
  struct P t = {x, x + 1, x + 2, x + 3};
  int r = use(&t);
  t.a = 99;
  return r + say(8) + x;
}

/* between two calls that read t: kept */
int case_store_between(int x)
{
  struct P t = {x, x + 1, x + 2, x + 3};
  int r = use(&t);
  t.b = x * 5;
  t.c = r & 7;
  return r * 3 + use(&t);
}

/* in a loop: the next trip's call reads it */
int case_store_loop(int n)
{
  struct P t = {1, 2, 3, 4};
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    s += use(&t);
    t.c = i * 2;
  }
  return s;
}

/* ---- O1 in entry_store_prop: read-only calls do not change an object ---- */

/* handed only to a read-only parameter: its fields stay the constants */
int case_const_in_loop(int n)
{
  struct P t = {1, 2, 3, 4};
  int s = 0;
  for (int i = 0; i < n; i++)
    s += use(&t) + t.a + t.d;
  return s;
}

/* handed to a parameter that writes: reloaded every trip */
int case_written_in_loop(int n)
{
  struct P t = {1, 2, 3, 4};
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    bump(&t);
    s += t.b + t.a;
  }
  return s;
}

/* read-only call, but written by name in the loop */
int case_named_write_in_loop(int n)
{
  struct P t = {1, 2, 3, 4};
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    s += use(&t) + t.a;
    t.a = s & 15;
  }
  return s;
}

/* ---- O4 in LICM: an exclusive parameter's field read in a loop ---- */
void bumpi(inout int * view p);

/* hoisted: the loop stores through another pointer and calls say() */
int case_licm_field(struct B * restrict view b, inout int * view dst, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    dst[i & 3] = b->cap * i;
    s += say(i) + b->cap;
  }
  return s;
}

/* kept: the loop writes the field through the parameter */
int case_licm_self(inout struct B * restrict view b, inout int * view dst, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    dst[i & 3] = b->cap;
    s += b->cap;
    b->cap += 2;
  }
  return s;
}

/* kept: through a pointer derived from it */
int case_licm_derived(inout int * restrict view d, inout int * view dst, int k, int n)
{
  int s = 0;
  int * view e = d + k;
  for (int i = 0; i < n; i++)
  {
    dst[i & 3] = d[1];
    s += d[1];
    e[0] += 3;
  }
  return s;
}

/* kept: handed to a call that writes */
int case_licm_handed(inout int * restrict view d, inout int * view dst, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    dst[i & 3] = d[0];
    s += d[0];
    bumpi(d);
  }
  return s;
}
