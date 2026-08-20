/* Post-allocation copy propagation and redundant reload elimination.
 *
 * Both passes run after register allocation is frozen (source/ir/regalloc.c,
 * ra_copy_propagate / ra_redundant_reload_elim) and both rewrite or delete
 * instructions that name physical registers, so a mistake here is a silent
 * wrong-value miscompile rather than a crash.  The soft-float library is the
 * shape they were written for and the ir_tests corpus is otherwise blind to
 * it: one union-punned 64-bit value re-read by a dozen inlined accessors,
 * across branches.
 *
 * Verified to exercise both passes: with them disabled this file compiles to
 * 592 bytes of .text, with them enabled 572.  What that covers is the shape
 * they actually fire on -- the union round-trip (`u.d = x; bits = u.u`) whose
 * store and reload land in the SAME register pair, and the repeated 64-bit
 * accessor reads the allocator binds into fresh pairs -- plus sub-word and
 * 32/64 mixed reads, which must keep their narrowing.  A miscompile shows up
 * as wrong sign/exp/mant words.
 *
 * `redef` and `escaped` pin the cases the passes must REFUSE (a source
 * redefined under the copy's uses; a punned slot whose address escapes to a
 * callee that rewrites it).  Be aware these do NOT currently have teeth:
 * disabling either refusal leaves this file, the soft-float library and the
 * whole ir_tests corpus byte-identical, because no input yet reaching the
 * passes produces the shape.  They are kept as documentation of the hazard
 * and as a starting point if one is ever found. */

extern int printf(const char *, ...);

typedef unsigned long long u64;
typedef unsigned int u32;

static void bump(u64 *p) { *p += 0x100000000ULL; }

static int dsign(u64 b) { return (int)(b >> 63); }
static int dexp(u64 b) { return (int)((b >> 52) & 0x7FF); }
static u64 dmant(u64 b) { return b & 0xFFFFFFFFFFFFFULL; }
static u32 dlo(u64 b) { return (u32)b; }
static unsigned char dbyte(u64 b) { return (unsigned char)(b >> 8); }

/* The soft-float opening: pun, then read the same value many times. */
static void classify(double d, int *out)
{
  union { double d; u64 u; } ua;
  ua.d = d;
  u64 bits = ua.u;

  out[0] = dsign(bits);
  out[1] = dexp(bits);
  out[2] = (int)(dmant(bits) >> 32);
  out[3] = (int)dlo(bits);
  out[4] = (int)dbyte(bits);

  /* Same reads again on the far side of a branch, so the copies span one. */
  if (dexp(bits) == 0x7FF)
    out[5] = dmant(bits) ? 1 : 2;
  else
    out[5] = dsign(bits) ? 3 : 4;

  out[6] = dexp(bits) + dsign(bits);
}

/* The source is redefined while copies of it are still live. */
static u64 redef(u64 a, int k)
{
  u64 acc = a;
  u64 first = acc + 1;
  if (k)
    acc = acc * 3ULL + 7ULL; /* redefines the propagation source */
  return first + acc + (acc >> 32);
}

/* The punned slot's address escapes to a callee that rewrites it. */
static u64 escaped(double d)
{
  union { double d; u64 u; } ua;
  ua.d = d;
  bump(&ua.u);
  return ua.u;
}

int main(void)
{
  int o[7];
  int i;

  classify(-2.5, o);
  printf("a=");
  for (i = 0; i < 7; i++) printf("%d,", o[i]);
  printf("\n");

  classify(1.0, o);
  printf("b=");
  for (i = 0; i < 7; i++) printf("%d,", o[i]);
  printf("\n");

  classify(0.0, o);
  printf("c=");
  for (i = 0; i < 7; i++) printf("%d,", o[i]);
  printf("\n");

  printf("redef0=%016llx\n", (unsigned long long)redef(0x0123456789ABCDEFULL, 0));
  printf("redef1=%016llx\n", (unsigned long long)redef(0x0123456789ABCDEFULL, 1));
  printf("esc=%016llx\n", (unsigned long long)escaped(2.5));
  printf("OK\n");
  return 0;
}
