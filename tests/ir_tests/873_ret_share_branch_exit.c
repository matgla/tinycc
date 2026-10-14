/* Return-block register sharing in the linear scan (regalloc_scan.c,
 * ra_ret_tail_closed): a RETURNVALUE feeder may borrow the register of a
 * still-live interval only when no path from its definition leaves the return
 * tail.  The Zig error pattern `p = r.payload; if (r.error) return r.error;`
 * has a feeder chain (load, copies, test) whose coalesced range covers the
 * branch to the code that still reads `p`; sharing r0 there clobbered `p`
 * before the stores (`ldr r0,[sp]; ldrh r0,[sp,#4]; cbz r0; ...; str r9,[r0]`).
 * It shows once ext_elim turns the error's re-extension into plain copies the
 * coalescer merges (zig.c ast-check segfaults).  TCC_DISABLE_PASS=
 * ra:ret_share_exit drops the guard and brings the failure back. */
#include <stdint.h>
#include <stdio.h>

typedef uint16_t u16;
typedef uint32_t u32;

struct EV
{
  u16 error;
};
struct Node
{
  u32 *parent;
  u32 *gen;
  u32 name;
  u32 inst;
  u32 tok;
  u32 *flag;
  u32 used;
};
struct EP
{
  struct Node *payload;
  u16 error;
};

#define NOINLINE __attribute__((noinline))

static struct Node pool[4];
static u32 want_error[4];
static int calls;

NOINLINE struct EP create(void *a)
{
  struct EP r;
  (void)a;
  r.payload = &pool[calls & 3];
  r.error = (u16)want_error[calls & 3];
  calls++;
  return r;
}

NOINLINE struct EV f(void *alloc, u32 *par, u32 *gz, u32 name, u32 inst, u32 tok, u32 *fl, int k)
{
  struct EP r = create(alloc);
  struct EV e;
  if (r.error)
  {
    u16 e1 = r.error;
    u16 e2 = e1;
    e.error = e2;
    return e;
  }
  struct Node *n = r.payload;
  n->parent = par;
  n->gen = gz;
  n->name = name;
  n->inst = inst;
  n->tok = tok;
  n->flag = fl;
  n->used = 0xffffffffu;
  if (k)
  {
    struct EP q = create(alloc);
    if (q.error)
    {
      u16 g1 = q.error;
      u16 g2 = g1;
      e.error = g2;
      return e;
    }
    q.payload->name = name ^ 0x55;
    q.payload->tok = tok ^ 0xaa;
  }
  e.error = 0;
  return e;
}

static void dump(const char *tag, struct EV e)
{
  printf("%s err=%u\n", tag, (unsigned)e.error);
  for (int i = 0; i < 2; i++)
    printf("  node%d name=%u inst=%u tok=%u used=%08x\n", i, (unsigned)pool[i].name, (unsigned)pool[i].inst,
           (unsigned)pool[i].tok, (unsigned)pool[i].used);
}

int main(void)
{
  u32 par = 1, gz = 2, fl = 3;
  struct EV e;

  calls = 0;
  want_error[0] = 0;
  want_error[1] = 0;
  e = f(0, &par, &gz, 10, 20, 30, &fl, 1);
  dump("ok-ok", e);

  calls = 0;
  want_error[0] = 7;
  e = f(0, &par, &gz, 11, 21, 31, &fl, 1);
  dump("err-first", e);

  calls = 0;
  want_error[0] = 0;
  want_error[1] = 0x1239;
  e = f(0, &par, &gz, 12, 22, 32, &fl, 1);
  dump("err-second", e);

  calls = 0;
  want_error[0] = 0;
  want_error[1] = 0;
  e = f(0, &par, &gz, 13, 23, 33, &fl, 0);
  dump("no-second", e);
  return 0;
}
