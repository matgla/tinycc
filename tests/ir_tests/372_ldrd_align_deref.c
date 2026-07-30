/* Guard for LDRD/STRD pairing of 64-bit register-deref accesses.
 *
 * svalue_to_iroperand marks a 64-bit lvalue align4_ok when its access chain
 * never crossed a packed member (SValue.underaligned, set at member access and
 * propagated through pointer arithmetic by gen_op).  The backend then lowers
 * `*p` / `p[i]` for long long / double through LDRD/STRD instead of two
 * 32-bit halves.  LDRD/STRD fault on ARMv7-M/v8-M when the address is not
 * 4-byte aligned, regardless of UNALIGN_TRP, so every packed-derived access
 * must stay on the LDR/STR-pair path:
 *
 *   pk_*        : packed member / nested member / array (const and variable
 *                 index, plus the *(a+i) spelling) — loads and stores through
 *                 a genuinely misaligned struct must not fault and must be
 *                 byte-exact.  The array cases cover the LOAD_INDEXED /
 *                 STORE_INDEXED lowering, whose 64-bit path historically
 *                 assumed alignment (pre-existing fault, fixed by the
 *                 underalign_hint transfer in the fusion passes).
 *   plain_*     : aligned pointer derefs — value round-trips through the new
 *                 LDRD/STRD path.
 *   vol_bump    : volatile double pointer walk (the 20021120-1 shape).
 */
#include <stdio.h>
#include <string.h>

struct __attribute__((packed)) PK
{
  char c;
  long long v;
  long long a[2];
};

struct I
{
  long long v;
};
struct __attribute__((packed)) OUTER
{
  char c;
  struct I i;
};

__attribute__((noinline)) long long pk_direct(struct PK *s) { return s->v; }
__attribute__((noinline)) long long pk_arr_const(struct PK *s) { return s->a[1]; }
__attribute__((noinline)) long long pk_arr_var(struct PK *s, int i) { return s->a[i]; }
__attribute__((noinline)) long long pk_arith(struct PK *s) { return *(s->a + 1); }
__attribute__((noinline)) long long pk_nested(struct OUTER *o) { return o->i.v; }
__attribute__((noinline)) void pk_st_direct(struct PK *s, long long x) { s->v = x; }
__attribute__((noinline)) void pk_st_arr(struct PK *s, int i, long long x) { s->a[i] = x; }

__attribute__((noinline)) long long plain_ld(long long *p) { return *p; }
__attribute__((noinline)) void plain_st(long long *p, long long x) { *p = x; }
__attribute__((noinline)) double plain_dld(double *p) { return *p; }
__attribute__((noinline)) void plain_dcp(double *q, double *p)
{
  q[0] = p[0];
  q[1] = p[1];
}

__attribute__((noinline)) double vol_bump(volatile double *p)
{
  double a = *p++;
  double b = *p++;
  double c = *p++;
  return a + b + c;
}

/* Backing storage that puts the packed struct at a deliberately odd address:
 * buf+1 makes s->v land at offset 2 and s->a at offset 10 — both 2 mod 4. */
static unsigned char buf[64];

int main(void)
{
  struct PK tmp;
  tmp.c = 0x5A;
  tmp.v = 0x1122334455667788LL;
  tmp.a[0] = 0x0102030405060708LL;
  tmp.a[1] = 0x090A0B0C0D0E0F10LL;
  memcpy(buf + 1, &tmp, sizeof tmp);
  struct PK *s = (struct PK *)(buf + 1);

  if (pk_direct(s) != 0x1122334455667788LL)
  {
    printf("FAIL pk_direct\n");
    return 1;
  }
  if (pk_arr_const(s) != 0x090A0B0C0D0E0F10LL)
  {
    printf("FAIL pk_arr_const\n");
    return 2;
  }
  if (pk_arr_var(s, 0) != 0x0102030405060708LL || pk_arr_var(s, 1) != 0x090A0B0C0D0E0F10LL)
  {
    printf("FAIL pk_arr_var\n");
    return 3;
  }
  if (pk_arith(s) != 0x090A0B0C0D0E0F10LL)
  {
    printf("FAIL pk_arith\n");
    return 4;
  }

  pk_st_direct(s, 0x7766554433221100LL);
  if (pk_direct(s) != 0x7766554433221100LL || buf[0] != 0)
  {
    printf("FAIL pk_st_direct\n");
    return 5;
  }
  pk_st_arr(s, 1, 0x0F0E0D0C0B0A0908LL);
  if (pk_arr_const(s) != 0x0F0E0D0C0B0A0908LL)
  {
    printf("FAIL pk_st_arr\n");
    return 6;
  }

  struct OUTER otmp;
  otmp.c = 1;
  otmp.i.v = 0x2468ACE013579BDFLL;
  memcpy(buf + 1, &otmp, sizeof otmp);
  if (pk_nested((struct OUTER *)(buf + 1)) != 0x2468ACE013579BDFLL)
  {
    printf("FAIL pk_nested\n");
    return 7;
  }

  long long ll = 0x0123456789ABCDEFLL;
  if (plain_ld(&ll) != 0x0123456789ABCDEFLL)
  {
    printf("FAIL plain_ld\n");
    return 8;
  }
  plain_st(&ll, 0xFEDCBA9876543210LL);
  if (ll != 0xFEDCBA9876543210LL)
  {
    printf("FAIL plain_st\n");
    return 9;
  }

  double d[2] = {1.5, -2.25}, e[2] = {0, 0};
  if (plain_dld(&d[1]) != -2.25)
  {
    printf("FAIL plain_dld\n");
    return 10;
  }
  plain_dcp(e, d);
  if (e[0] != 1.5 || e[1] != -2.25)
  {
    printf("FAIL plain_dcp\n");
    return 11;
  }

  volatile double vd[3];
  vd[0] = 0.5;
  vd[1] = 1.25;
  vd[2] = 2.0;
  if (vol_bump(vd) != 3.75)
  {
    printf("FAIL vol_bump\n");
    return 12;
  }

  printf("OK\n");
  return 0;
}
