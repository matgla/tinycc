#include <stdio.h>
#include <string.h>

/* Struct-copy forwarding (ssa:copy_fwd, ssa:memmove_global_fwd) and dead
 * local-slot elimination must see every way an object's bytes stay reachable
 * when no operand names them:
 *   - memcpy/memmove/memset return their destination: a result in use is the
 *     object's address, read and written through later;
 *   - a pointer one past an object is still that object's;
 *   - a writable variable placed in a named section that also holds code
 *     lives in a section without SHF_WRITE (tcc only; gcc rejects the mix);
 *   - an MLA reads its accumulator from a fourth operand;
 *   - a jump into the middle of a straight-line window merges a path on which
 *     the copy never ran. */

struct big
{
  int a[40];
  int b[30];
};

#define NI __attribute__((noinline))

NI static void fill(struct big *t, int v)
{
  for (int i = 0; i < 40; i++)
    t->a[i] = v + i;
  for (int i = 0; i < 30; i++)
    t->b[i] = v * 2 + i;
}

NI static int dirty(int v)
{
  volatile int junk[1024];
  for (int i = 0; i < 1024; i++)
    junk[i] = v ^ (i * 2654435761u);
  return junk[v & 1023];
}

/* memcpy's result points at s, the frame source of d = s: the store through
 * it comes after the copy and must not show in d. */
NI static int src_via_memcpy_ret(int v)
{
  struct big x, s, d;
  fill(&x, v);
  int *p = memcpy(&s, &x, sizeof s);
  d = s;
  p[3] = 31337;
  return d.a[3] * 10 + s.a[3] % 7;
}

/* ... the destination side: e = d reads d, which p (memcpy's result) writes. */
NI static int dst_via_memcpy_ret(int v, int n)
{
  struct big s, d, e;
  fill(&s, v);
  int *p = memcpy(&d, &s, sizeof d);
  e = d;
  for (int i = 0; i < n; i++)
    p[i] = -i;
  return e.a[2] * 100 + d.a[2];
}

/* A later partial memcpy's result reads the bytes an earlier full copy left
 * in d: that copy has a reader no operand shows. */
NI static int dead_copy_read_via_memcpy_ret(int v)
{
  struct big s, d;
  int x[2] = {v * 7, v * 9};
  fill(&s, v);
  d = s;
  int *p = memcpy(&d, x, 8);
  return p[5] * 1000 + p[0];
}

/* memcpy into a member: its result points into d, which f = d copies. */
NI static int memcpy_ret_into_member(int v)
{
  struct big s, d, e;
  fill(&s, v);
  fill(&e, v + 50);
  d = s;
  int *p = memcpy(&d.b[0], &e.b[0], 4 * 4);
  struct big f = d;
  p[1] = 1234;
  return f.b[1] * 10000 + d.b[1];
}

/* The result read after another copy overwrote its object. */
NI static int ret_then_overwrite(struct big *s1, struct big *s2)
{
  struct big d;
  struct big *p = memcpy(&d, s1, sizeof d);
  int r0 = d.a[2];
  d = *s2;
  return p->a[1] * 10 + r0 + d.a[3] * 1000;
}

NI static int ret_then_overwrite_frame(int k)
{
  struct big d, s1, s2;
  fill(&s1, k);
  fill(&s2, 100 + k);
  struct big *p = memcpy(&d, &s1, sizeof d);
  int r0 = d.a[2];
  d = s2;
  return p->a[1] * 10 + r0 + d.a[3] * 1000;
}

/* Only the result reads the copy: the copy is no dead store. */
NI static int ret_only_reader(struct big *s1, struct big *s2)
{
  struct big d;
  struct big *p = memcpy(&d, s1, sizeof d);
  d = *s2;
  return p->a[1];
}

/* memcpy's result passed on and summed: the copy into t must stay. */
NI static int sum_ptr(const struct big *t)
{
  int r = 0;
  for (int i = 0; i < 40; i++)
    r += t->a[i] * (i + 1);
  return r;
}
NI static int ret_as_argument(const struct big *p)
{
  struct big t;
  return sum_ptr(memcpy(&t, p, sizeof t));
}

/* memset's result too. */
NI static int memset_ret(int v)
{
  struct big s, d;
  fill(&s, v);
  d = s;
  int *p = memset(&d.a[4], 0, 8);
  p[0] += 9;
  return d.a[4] * 100 + d.a[5] + d.a[6];
}

/* A pointer one past d is d's: the callee steps back into it. */
NI static void poke_end(char *end) { ((struct big *)(end - sizeof(struct big)))->a[2] = 99; }
NI static int one_past_end(struct big *p, int k)
{
  struct big s = *p;
  s.a[2] += k;
  struct big d = s;
  poke_end((char *)&d + sizeof d);
  return d.a[2] * 100 + s.a[2];
}

/* A variable in a named section shared with code: not SHF_WRITE, but written
 * at run time (gcc rejects the mix, so the host build keeps it in .data). */
#ifdef __TINYC__
#define CODE_SECTION __attribute__((section(".text.cfwmix")))
#else
#define CODE_SECTION
#endif
CODE_SECTION NI int mixed_code(int x) { return x + 1; }
CODE_SECTION struct big g_mixed = {{1, 2, 3}};
NI static void bump_mixed(void) { g_mixed.a[1] += 100; }
NI static int from_mixed_section(void)
{
  struct big d = g_mixed;
  bump_mixed();
  return d.a[1] * 1000 + g_mixed.a[1];
}

/* A store into another frame object whose address is out cannot write the
 * frame source (escaped too, through fill): d's reads still go to s. */
static struct big *g_other;
NI static void keep_other(struct big *t) { g_other = t; }
NI static int other_escaped_write(int v)
{
  struct big s, d, e;
  fill(&s, v);
  fill(&e, v + 1);
  keep_other(&e);
  d = s;
  e.a[3] = 77;
  int r = d.a[3] * 1000 + d.a[4];
  keep_other(0);
  return r + e.a[3] + e.a[4];
}

/* ssa:memmove_global_fwd forwards `l = global` copies of up to 256 bytes. */
struct s12
{
  int a[12];
};
static const struct s12 k_const = {{3, -4, 5, 6, -7, 8, 9, 10, -11, 12, 13, 14}};
static unsigned acc = 7;
#define MIX(x) (acc = acc * 31 + (unsigned)(x))

/* The copy's slot read only as an MLA accumulator (acc * 31 + l.a[k]). */
NI static void mla_accumulator(int v)
{
  struct s12 l;
  for (int z = 0; z < 12; z++)
    l.a[z] = v * 3 + z;
  MIX(l.a[2]);
  l = k_const;
  MIX(l.a[6]);
  MIX(l.a[0]);
}

/* l0 = G in one arm only: the reads after the merge must not all read G. */
static struct s12 g_src;
static struct s12 *g_hold;
NI static struct s12 mk(int v)
{
  struct s12 r;
  for (int i = 0; i < 12; i++)
    r.a[i] = v * 7 + i;
  if (g_hold)
    g_hold->a[1] += 3;
  return r;
}
NI static void bump(int k, int v)
{
  if (g_hold)
    g_hold->a[k] = v;
}
NI static void copy_on_one_arm(struct s12 *p, int v)
{
  struct s12 l0, l1, l2, l3;
  for (int z = 0; z < 12; z++)
    l0.a[z] = v * 2 + z;
  for (int z = 0; z < 12; z++)
    l1.a[z] = v * 3 + z;
  for (int z = 0; z < 12; z++)
    l2.a[z] = v * 4 + z;
  for (int z = 0; z < 12; z++)
    l3.a[z] = v * 5 + z;
  {
    struct s12 *pp = &l3;
    pp->a[4] = 4;
  }
  p->a[6] = v + 9;
  bump(4, v + 8);
  MIX(l3.a[7]);
  {
    struct s12 *pp = &l1;
    pp->a[6] = -1;
  }
  if (v & 1)
  {
    MIX(l2.a[11]);
    bump(5, v + 5);
  }
  else
  {
    l1 = mk(v + 3);
    l0 = g_src;
  }
  l2 = g_src;
  MIX(l0.a[3]);
  MIX(l0.a[10]);
  MIX(l1.a[8]);
  MIX(l1.a[8]);
  MIX(l2.a[4]);
  MIX(l2.a[11]);
  MIX(l3.a[9]);
  MIX(l3.a[5]);
}

int main(void)
{
  struct big x, y;
  fill(&x, 0);
  fill(&y, 1000);
  for (int i = 0; i < 12; i++)
    g_src.a[i] = 40 - i;
  dirty(0x5a5a);
  printf("src_via_memcpy_ret %d\n", src_via_memcpy_ret(10));
  printf("dst_via_memcpy_ret %d\n", dst_via_memcpy_ret(10, 4));
  dirty(0x1234);
  printf("dead_copy_read_via_memcpy_ret %d\n", dead_copy_read_via_memcpy_ret(10));
  printf("memcpy_ret_into_member %d\n", memcpy_ret_into_member(10));
  dirty(0x777);
  printf("ret_then_overwrite %d\n", ret_then_overwrite(&x, &y));
  printf("ret_then_overwrite_frame %d\n", ret_then_overwrite_frame(3));
  dirty(0x4321);
  printf("ret_only_reader %d\n", ret_only_reader(&x, &y));
  dirty(0x1111);
  printf("ret_as_argument %d\n", ret_as_argument(&y));
  printf("memset_ret %d\n", memset_ret(10));
  printf("one_past_end %d\n", one_past_end(&x, 5));
  printf("other_escaped_write %d\n", other_escaped_write(10));
  printf("from_mixed_section %d %d\n", from_mixed_section(), mixed_code(1));
  mla_accumulator(0);
  printf("mla_accumulator %u\n", acc);
  mla_accumulator(8);
  printf("mla_accumulator %u\n", acc);
  for (int v = 0; v < 4; v++)
  {
    struct s12 t = g_src;
    copy_on_one_arm(&t, v);
    printf("copy_on_one_arm %d %u\n", v, acc);
  }
  return 0;
}
