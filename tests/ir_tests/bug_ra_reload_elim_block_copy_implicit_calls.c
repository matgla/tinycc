/* ra:reload_elim must forget "slot S holds registers rA:rB" across every op
 * that clobbers registers or writes the frame without naming them as its IR
 * destination.
 *
 * ra_redundant_reload_elim drops a `LOAD d <- S` when d was allocated the
 * registers a previous `STORE S <- v` stored from.  Its kill set used to be an
 * allow-list of the ops it knew could disturb a slot or a register, so:
 *
 *   - a BLOCK_COPY (an aggregate initializer or assignment) kept the fact
 *     although its lowering uses r0-r3/r12/lr as cursors and data registers,
 *     and although it may overwrite the tracked slot itself;
 *   - a soft-float operation lowered to an __aeabi_* helper kept it although
 *     the helper clobbers r0-r3 whatever register its result is moved to.
 *
 * The union puns (double -> long long through the frame) are what the pass
 * exists for, so they are the shape that exposes it.  Every function is
 * checked against values computed independently of the punned slot.
 * Wrong at -O1/-O2 (and -Os where the copy is inlined), fine at -O0. */
#include <stdio.h>

typedef union
{
  double d;
  long long u;
} U;

static volatile int vk = 7;
static volatile double va = 1.5;
static volatile double vb = 4.0;
static volatile double vc = 0.25;

/* BLOCK_COPY of an array initializer between the store and the reload.
 * The copy's cursors are r0/r1, the very registers holding x.d. */
__attribute__((noinline)) long long bc_clobbers_regs(double a, int k)
{
  U x;
  x.d = a;
  int arr[40] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40};
  arr[5] = k;
  long long r = x.u;
  return r + arr[k];
}

/* The same with a copy small enough to stay an inline LDM/STM sequence. */
__attribute__((noinline)) long long bc_small_clobbers_regs(double a, int k)
{
  U x;
  x.d = a;
  int arr[12] = {101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112};
  arr[3] = k;
  long long r = x.u;
  return r + arr[k];
}

/* A BLOCK_COPY that overwrites the tracked slot: the register still holds the
 * old x.d, memory holds the copied-in bytes, and the reload must read memory. */
typedef struct
{
  long long head;
  int rest[38];
} Big;

typedef union
{
  double d;
  Big b;
} UB;

__attribute__((noinline)) long long bc_overwrites_slot(double a, const Big *src)
{
  UB x;
  x.d = a;
  x.b = *src;
  long long r = x.b.head;
  return r + x.b.rest[3];
}

typedef struct
{
  long long head;
  int rest[6];
} Small;

typedef union
{
  double d;
  Small s;
} US;

__attribute__((noinline)) long long bc_small_overwrites_slot(double a, const Small *src)
{
  US x;
  x.d = a;
  x.s = *src;
  long long r = x.s.head;
  return r + x.s.rest[2];
}

/* Soft-float helper calls (__aeabi_dmul, __aeabi_ddiv, __aeabi_dadd,
 * __aeabi_i2d, __aeabi_d2iz) between the store and the reload. */
__attribute__((noinline)) long long fp_mul_clobbers_regs(double a, double b, double c)
{
  U x;
  x.d = a;
  double t = b * c;
  long long r = x.u;
  return r + (long long)(int)t;
}

__attribute__((noinline)) long long fp_div_clobbers_regs(double a, double b, double c)
{
  U x;
  x.d = a;
  double t = b / c;
  long long r = x.u;
  return r ^ (long long)(int)t;
}

__attribute__((noinline)) long long fp_cvt_clobbers_regs(double a, int n)
{
  U x;
  x.d = a;
  double t = (double)n;
  long long r = x.u;
  return r + (long long)(int)(t + 0.5);
}

__attribute__((noinline)) long long fp_cvt_back_clobbers_regs(double a, double b)
{
  U x;
  x.d = a;
  int t = (int)b;
  long long r = x.u;
  return r + t;
}

static long long bits(double d)
{
  U x;
  x.d = d;
  return x.u;
}

static int check(const char *name, long long got, long long want)
{
  if (got != want)
  {
    printf("%s: got %lld want %lld\n", name, got, want);
    return 1;
  }
  printf("%s ok\n", name);
  return 0;
}

int main(void)
{
  int bad = 0;
  double a = va, b = vb, c = vc;
  int k = vk;
  long long ab = bits(a);

  bad += check("bc_clobbers_regs", bc_clobbers_regs(a, k), ab + 8);
  bad += check("bc_small_clobbers_regs", bc_small_clobbers_regs(a, 4), ab + 105);

  Big big;
  for (int i = 0; i < 38; i++)
    big.rest[i] = 1000 + i;
  big.head = 0x1122334455667788LL;
  bad += check("bc_overwrites_slot", bc_overwrites_slot(a, &big), 0x1122334455667788LL + 1003);

  Small small;
  for (int i = 0; i < 6; i++)
    small.rest[i] = 50 + i;
  small.head = 0x0102030405060708LL;
  bad += check("bc_small_overwrites_slot", bc_small_overwrites_slot(a, &small), 0x0102030405060708LL + 52);

  bad += check("fp_mul_clobbers_regs", fp_mul_clobbers_regs(a, b, c), ab + 1);
  bad += check("fp_div_clobbers_regs", fp_div_clobbers_regs(a, b, c), ab ^ 16);
  bad += check("fp_cvt_clobbers_regs", fp_cvt_clobbers_regs(a, 9), ab + 9);
  bad += check("fp_cvt_back_clobbers_regs", fp_cvt_back_clobbers_regs(a, b), ab + 4);
  return bad ? 1 : 0;
}
