/* Copies of never-written bytes (frame_dfe.c, undefined values).
 *
 * Zig builds a tagged value in a temporary whose payload union is wider than
 * the member written, then moves the whole temporary through two more
 * temporaries into an argument array:
 *
 *     t10.tag = 0; t10.payload.u = x; t13 = t10; t8 = t13; arr[i] = t8;
 *
 * The union's tail was never written, so its words are loads of a slot
 * nothing stores to (or reads of a vreg nothing defines) and the stores into
 * the array only copy garbage.  They are dropped; every byte that WAS written
 * on any path, through a pointer, by a callee or by a volatile access must
 * still arrive.  Each function prints only bytes that are defined.  */
#include <stdio.h>
#include <string.h>

typedef unsigned long long u64;
typedef unsigned char u8;

typedef struct
{
  u8 tag;
  union
  {
    u64 u;
    double f;
    struct { const char *p; unsigned n; } s;
    unsigned w[4];
  } payload;
} Value;

struct Slice { const Value *p; unsigned n; };

__attribute__((noinline)) unsigned sum_defined(struct Slice a)
{
  unsigned s = 0;
  for (unsigned i = 0; i < a.n; i++)
    s = s * 31u + a.p[i].tag + (unsigned)a.p[i].payload.u + (unsigned)(a.p[i].payload.u >> 32);
  return s;
}

/* The zig shape. */
static Value make_u(unsigned x)
{
  Value t10, t13;
  t10.tag = 0;
  t10.payload.u = x;
  t13 = t10;
  return t13;
}

__attribute__((noinline)) unsigned zig_shape(unsigned a, unsigned b)
{
  Value arr[2];
  Value t8;
  t8 = make_u(a);
  arr[0] = t8;
  t8 = make_u(b);
  arr[1] = t8;
  struct Slice s = {arr, 2};
  return sum_defined(s);
}

/* The same shape inside a loop: one array element built per iteration. */
__attribute__((noinline)) unsigned in_loop(unsigned n)
{
  unsigned acc = 0;
  for (unsigned i = 0; i < n; i++)
  {
    Value arr[1];
    Value t8 = make_u(i * 7u + 1u);
    arr[0] = t8;
    struct Slice s = {arr, 1};
    acc = acc * 17u + sum_defined(s);
  }
  return acc;
}

/* A word written on ONE path only: when the path is taken the word must
   reach the copy. */
__attribute__((noinline)) unsigned maybe_written(int c, unsigned a)
{
  Value t10;
  t10.tag = 3;
  t10.payload.u = a;
  if (c)
    t10.payload.w[3] = 0x77665544u;
  Value t13 = t10;
  Value arr[1];
  arr[0] = t13;
  /* defined only when c */
  unsigned tail = c ? arr[0].payload.w[3] : 0u;
  struct Slice s = {arr, 1};
  return sum_defined(s) + tail;
}

/* The tail written through a pointer a callee got. */
__attribute__((noinline)) void fill_tail(Value *v, unsigned x) { v->payload.w[2] = x; v->payload.w[3] = x + 1u; }

__attribute__((noinline)) unsigned written_by_callee(unsigned a)
{
  Value t10;
  t10.tag = 9;
  t10.payload.u = a;
  fill_tail(&t10, 100u + a);
  Value t13 = t10;
  Value arr[1];
  arr[0] = t13;
  return arr[0].payload.w[2] * 1000u + arr[0].payload.w[3] + sum_defined((struct Slice){arr, 1});
}

/* Bytes written one at a time, the rest never. */
__attribute__((noinline)) unsigned byte_writes(unsigned a)
{
  Value t10;
  t10.tag = 1;
  t10.payload.u = a;
  ((u8 *)&t10.payload.w[2])[1] = (u8)(a + 5u);
  Value t13 = t10;
  Value arr[1];
  arr[0] = t13;
  return ((u8 *)&arr[0].payload.w[2])[1] * 7u + sum_defined((struct Slice){arr, 1});
}

/* Undefined value used by arithmetic as well as stored: the arithmetic result
   is only printed as "taken", never its value. */
__attribute__((noinline)) unsigned undef_in_arith(unsigned a)
{
  Value t10;
  t10.tag = 2;
  t10.payload.u = a;
  Value t13 = t10;
  unsigned junk = t13.payload.w[3] + 1u; /* indeterminate */
  (void)junk;
  Value arr[1];
  arr[0] = t13;
  return sum_defined((struct Slice){arr, 1});
}

/* A volatile access keeps its object. */
__attribute__((noinline)) unsigned volatile_tail(unsigned a)
{
  volatile Value t10;
  t10.tag = 4;
  t10.payload.u = a;
  t10.payload.w[3] = 0x01020304u;
  Value t13;
  t13.tag = t10.tag;
  t13.payload.u = t10.payload.u;
  t13.payload.w[3] = t10.payload.w[3];
  Value arr[1];
  arr[0] = t13;
  return arr[0].payload.w[3] + sum_defined((struct Slice){arr, 1});
}

/* A struct with an unwritten MIDDLE member: the neighbours still move. */
struct mid { unsigned a; unsigned never; unsigned c; unsigned d; };
__attribute__((noinline)) struct mid copy_mid(unsigned a, unsigned c, unsigned d)
{
  struct mid m, n;
  m.a = a;
  m.c = c;
  m.d = d;
  n = m;
  struct mid o = n;
  return o;
}

/* Registers: a copy whose source words are plain locals never assigned. */
__attribute__((noinline)) unsigned partial_locals(unsigned a, unsigned b)
{
  unsigned x, y;
  Value t8;
  t8.tag = 5;
  t8.payload.w[0] = a;
  t8.payload.w[1] = b;
  (void)x;
  (void)y;
  Value arr[1];
  arr[0] = t8;
  return arr[0].payload.w[0] * 3u + arr[0].payload.w[1];
}

int main(void)
{
  printf("zig %u %u\n", zig_shape(5, 9), zig_shape(0x10000u, 0xffffffffu));
  printf("loop %u %u\n", in_loop(1), in_loop(6));
  printf("maybe %u %u\n", maybe_written(1, 4), maybe_written(0, 4));
  printf("callee %u\n", written_by_callee(8));
  printf("bytes %u\n", byte_writes(20));
  printf("arith %u\n", undef_in_arith(33));
  printf("volatile %u\n", volatile_tail(12));
  struct mid m = copy_mid(1, 2, 3);
  printf("mid %u %u %u\n", m.a, m.c, m.d);
  printf("partial %u\n", partial_locals(10, 20));
  return 0;
}
