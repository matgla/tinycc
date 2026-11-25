/* -b cases (test_bcheck.py): argv[1] picks one; each prints "go <case>"
   first and "ok <case> <value>" if nothing stopped it. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int garr[8];
int gafter = 7;
struct S
{
  int a[4];
  int tail;
};
struct S gs;

static int sink;
static volatile int idx;

static int local_write(int i)
{
  int a[4];
  int b[4] = {1, 2, 3, 4};
  memset(a, 0, sizeof a);
  a[i] = 42;
  return a[0] + b[0];
}

static int local_read(int i)
{
  int a[4] = {1, 2, 3, 4};
  return a[i];
}

static int global_write(int i)
{
  garr[i] = 5;
  return garr[0] + gafter;
}

static int struct_member(int i)
{
  struct S s;
  memset(&s, 0, sizeof s);
  s.a[i] = 3;
  gs.a[i] = 4;
  return s.a[0] + s.tail + gs.a[0];
}

static int heap_write(int i)
{
  int *p = malloc(4 * sizeof(int));
  int r;
  p[i] = 9;
  r = p[0];
  free(p);
  return r;
}

static int ptr_walk(int n)
{
  char buf[16];
  char *p = buf, *end = buf + sizeof buf;
  int s = 0;
  memset(buf, 1, sizeof buf);
  while (p < end)
    s += *p++;
  /* one past the end is a valid pointer, end[-1] the last element */
  s += end[-1];
  p = buf;
  p += n;
  *p = 2; /* n == 16: one past the end, written */
  return s;
}

static int use_after_free(void)
{
  int *p = malloc(4 * sizeof(int));
  p[0] = 1;
  free(p);
  return p[0];
}

static int double_free(void)
{
  char *p = malloc(10);
  free(p);
  free(p);
  return 0;
}

static int realloc_ok(void)
{
  int *p = malloc(2 * sizeof(int));
  p[0] = 1, p[1] = 2;
  p = realloc(p, 8 * sizeof(int));
  p[7] = 3;
  int r = p[0] + p[1] + p[7];
  free(p);
  return r;
}

static int realloc_old(void)
{
  int *p = malloc(2 * sizeof(int));
  int *q = realloc(p, 8 * sizeof(int));
  q[0] = 1;
  return p[0]; /* the old block was freed by realloc */
}

static int calloc_over(int i)
{
  short *p = calloc(5, sizeof(short));
  p[i] = 1;
  return p[0];
}

static inline int inl_get(int i)
{
  int t[3] = {1, 2, 3};
  return t[i];
}

static int inline_twice(int i)
{
  return inl_get(0) * 10 + inl_get(i);
}

static int vla_sum(int n)
{
  int v[n];
  int s = 0;
  for (int i = 0; i < n; i++)
    v[i] = i;
  for (int i = 0; i < n; i++)
    s += v[i];
  return s;
}

static int str_len(void)
{
  const char *s = "hello";
  int n = 0;
  while (*s++)
    n++;
  return n;
}

static int struct_copy(void)
{
  struct S a, b;
  memset(&a, 0, sizeof a);
  a.a[1] = 2;
  a.tail = 5;
  b = a;
  gs = b;
  return gs.a[1] + gs.tail;
}

/* a local registered and left by an early return, then frames of other
   shapes over the same stack */
static int early(int k)
{
  char big[24];
  if (k)
    return 1;
  memset(big, 0, sizeof big);
  big[23] = 4;
  return big[23];
}

static int scalars_over(int k)
{
  int x = k, y = k + 1, *p = &x, *q = &y;
  *p += *q;
  return x;
}

static int frames(void)
{
  int r = early(1) + early(0);
  for (int i = 0; i < 3; i++)
    r += scalars_over(i) + local_write(1);
  return r;
}

/* a static written only through checked member stores: its reads must not
   fold to the initializer (tcc saw no direct store) */
static struct
{
  int a, b;
} st;
__attribute__((noinline)) static void set_st(int v)
{
  st.b = v;
}
__attribute__((noinline)) static int get_st(void)
{
  return st.b;
}

static int global_read(int i)
{
  return garr[i];
}

/* a pointer into a block that is gone, read far past its end: checked (and
   reported) only while the block's region is still registered */
static char *leak;
static int probe(void)
{
  volatile char *p = (volatile char *)leak;
  return p[40] * 0;
}

/* `big` is declared after the return; the pass of the loop before registered
   it, and its scope dropped it */
static int after_return(int n)
{
  int r = 0;
  for (int i = 0; i < 3; i++)
  {
    if (i == n)
      return r;
    char big[16];
    memset(big, i, sizeof big);
    leak = big;
    r += big[15];
  }
  return r;
}

static int scalar_after_return(int n)
{
  int r = 0;
  for (int i = 0; i < 3; i++)
  {
    if (i == n)
      return r;
    int x = i;
    int *p = &x;
    leak = (char *)p;
    r += *p;
  }
  return r;
}

/* VLAs and alloca */
static int vla_write(int n)
{
  int v[n];
  for (int i = 0; i <= n; i++)
    v[i] = i; /* v[n]: one past the end, written */
  return v[0];
}

static int vla_scope(int n)
{
  int s = 0;
  for (int k = 0; k < 2; k++)
  {
    int v[n];
    v[n - 1] = k + 1;
    s += v[n - 1];
    leak = (char *)v;
  }
  return s + probe();
}

static __attribute__((noinline)) int alloca_use(int n, int over)
{
  char *p = __builtin_alloca(n);
  memset(p, 1, n);
  leak = p;
  return p[n - 1 + over];
}

static int alloca_ok(int n)
{
  int r = alloca_use(n, 0);
  return r + probe(); /* the block died with alloca_use's frame */
}

/* a scalar whose address is taken */
static int scalar_addr(int i)
{
  int x = 3, y = 5;
  int *p = &x;
  p[i] += 1;
  return x + y;
}

static __attribute__((noinline)) int param_addr(int a, int i)
{
  int *p = &a;
  p[i] += 2;
  return a;
}

/* the C library's reads and writes */
static char src20[20] = "0123456789abcdefghi";
static int lib_calls(int which)
{
  int a[4];
  char d[6], s[4] = {'a', 'b', 'c', 'd'};
  int r = 0;
  switch (which)
  {
  case 0: /* all in bounds */
  {
    char *dup;
    memcpy(a, src20, sizeof a);
    memmove(a, (char *)a + 1, sizeof a - 1);
    memset(d, 0, sizeof d);
    strcpy(d, "abc");
    strcat(d, "de");
    strncpy(d, "xy", sizeof d);
    strncat(d, "zzzz", 3);
    dup = strdup(d);
    r = (int)strlen(d) + (strchr(d, 'z') - d) + !memcmp(dup, d, 6) + !strcmp(dup, "xyzzz") +
        !strncmp(dup, "xyzq", 3) + (strrchr(dup, 'z') - dup);
    free(dup);
    return r; /* 5 + 2 + 1 + 1 + 1 + 4 */
  }
  case 1:
    memcpy(a, src20, sizeof a + 4);
    break;
  case 2:
    memcpy(src20, a, sizeof a + 4); /* src20 is fine, a too short */
    break;
  case 3:
    memset(d, 0, sizeof d + 1);
    break;
  case 4:
    strcpy(d, "toolong");
    break;
  case 5:
    r = (int)strlen(s); /* no terminator in s */
    break;
  case 6:
    strcpy(d, "abc");
    strcat(d, "def");
    break;
  }
  return r + a[0] + d[0];
}

/* a struct copied whole: both regions */
static int struct_copy_over(int which)
{
  struct S t;
  struct S *p = malloc(sizeof(struct S) - sizeof(int));
  memset(&t, 0, sizeof t);
  if (which)
    t = *p; /* the source is short */
  else
    *p = t; /* the destination is short */
  free(p);
  return t.tail;
}

/* longjmp leaves deep()'s frame: its buffer's region goes with it */
static jmp_buf jb;
static __attribute__((noinline)) void deep(int k)
{
  char buf[16];
  memset(buf, 0, sizeof buf);
  leak = buf;
  if (k)
    longjmp(jb, 5);
}

static int jump(void)
{
  volatile int v = setjmp(jb);
  if (v == 0)
  {
    deep(1);
    return -1;
  }
  return v + probe();
}

/* main's argv: the array, its NULL included, and each string are regions
   (__bound_main_arg at main's entry) */
static int argv_use(int argc, char **argv, int which)
{
  switch (which)
  {
  case 1:
    return argv[argc + 1 + idx] != 0; /* past the NULL */
  case 2:
    return argv[1][strlen(argv[1]) + 1 + idx]; /* past the string's terminator */
  case 3:
    strcpy(argv[1], "longer than any case's name");
    return 0;
  }
  return (argv[argc] == 0) + (int)strlen(argv[1]) + argv[1][strlen(argv[1])];
}

int main(int argc, char **argv)
{
  const char *c = argc > 1 ? argv[1] : "local_ok";
  int r = 0;
  printf("go %s\n", c);
  fflush(stdout);
  if (!strcmp(c, "local_ok"))
    r = local_write(3) + local_read(2);
  else if (!strcmp(c, "local_write"))
    r = local_write(4);
  else if (!strcmp(c, "local_read"))
    r = local_read(4);
  else if (!strcmp(c, "local_under"))
    r = local_write(-1);
  else if (!strcmp(c, "global_ok"))
    r = global_write(7);
  else if (!strcmp(c, "global_write"))
    r = global_write(8);
  else if (!strcmp(c, "struct_ok"))
    r = struct_member(3);
  else if (!strcmp(c, "struct_write"))
    r = struct_member(5);
  else if (!strcmp(c, "heap_ok"))
    r = heap_write(3);
  else if (!strcmp(c, "heap_write"))
    r = heap_write(4);
  else if (!strcmp(c, "walk_ok"))
    r = ptr_walk(15);
  else if (!strcmp(c, "walk_end"))
    r = ptr_walk(16);
  else if (!strcmp(c, "use_after_free"))
    r = use_after_free();
  else if (!strcmp(c, "double_free"))
    r = double_free();
  else if (!strcmp(c, "realloc_ok"))
    r = realloc_ok();
  else if (!strcmp(c, "realloc_old"))
    r = realloc_old();
  else if (!strcmp(c, "calloc_ok"))
    r = calloc_over(4);
  else if (!strcmp(c, "calloc_write"))
    r = calloc_over(5);
  else if (!strcmp(c, "inline_ok"))
    r = inline_twice(2);
  else if (!strcmp(c, "inline_read"))
    r = inline_twice(3);
  else if (!strcmp(c, "vla_ok"))
    r = vla_sum(idx + 6);
  else if (!strcmp(c, "str_ok"))
    r = str_len();
  else if (!strcmp(c, "copy_ok"))
    r = struct_copy();
  else if (!strcmp(c, "frames_ok"))
    r = frames();
  else if (!strcmp(c, "static_ok"))
    set_st(9), r = get_st();
  else if (!strcmp(c, "global_read"))
    r = global_read(8);
  else if (!strcmp(c, "after_return_ok"))
    r = after_return(2), r += probe();
  else if (!strcmp(c, "scalar_return_ok"))
    r = scalar_after_return(2), r += probe();
  else if (!strcmp(c, "vla_write"))
    r = vla_write(idx + 5);
  else if (!strcmp(c, "vla_scope_ok"))
    r = vla_scope(idx + 3);
  else if (!strcmp(c, "alloca_ok"))
    r = alloca_ok(idx + 9);
  else if (!strcmp(c, "alloca_write"))
    r = alloca_use(idx + 9, 1);
  else if (!strcmp(c, "scalar_ok"))
    r = scalar_addr(idx) + param_addr(4, idx);
  else if (!strcmp(c, "scalar_write"))
    r = scalar_addr(idx + 1);
  else if (!strcmp(c, "param_write"))
    r = param_addr(4, idx + 1);
  else if (!strcmp(c, "lib_ok"))
    r = lib_calls(0);
  else if (!strcmp(c, "memcpy_dst"))
    r = lib_calls(1);
  else if (!strcmp(c, "memcpy_src"))
    r = lib_calls(2);
  else if (!strcmp(c, "memset_over"))
    r = lib_calls(3);
  else if (!strcmp(c, "strcpy_over"))
    r = lib_calls(4);
  else if (!strcmp(c, "strlen_unterm"))
    r = lib_calls(5);
  else if (!strcmp(c, "strcat_over"))
    r = lib_calls(6);
  else if (!strcmp(c, "copy_dst"))
    r = struct_copy_over(0);
  else if (!strcmp(c, "copy_src"))
    r = struct_copy_over(1);
  else if (!strcmp(c, "longjmp_ok"))
    r = jump();
  else if (!strcmp(c, "argv_ok"))
    r = argv_use(argc, argv, 0);
  else if (!strcmp(c, "argv_past"))
    r = argv_use(argc, argv, 1);
  else if (!strcmp(c, "argv_str_past"))
    r = argv_use(argc, argv, 2);
  else if (!strcmp(c, "argv_strcpy"))
    r = argv_use(argc, argv, 3);
  printf("ok %s %d\n", c, r);
  return 0;
}
