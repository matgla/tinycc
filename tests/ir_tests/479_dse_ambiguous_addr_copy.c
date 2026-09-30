/* DSE's write-only address tracking (dse.c, prop_tmp/prop_var): a temp that
   holds one of two stack addresses is "ambiguous" (-2), and a copy of it must
   stay ambiguous.  The copy used to drop it, so the loads through the copy
   marked nothing read, both addresses looked write-only, and DSE deleted the
   stores that filled the objects.

   The shape is tinycc's own arm-thumb-gen.c: thumb_emit_data_processing_mop64
   takes two handler structs by value and, once inlined into
   data_processing_mop_impl, picks one per half:

     const H *h = (half == 0) ? &regular : &carry_h;  ... h->reg(...) ...

   The self-hosted tcc then called through a stack address on every 64-bit
   add, sub or compare. */
#include <stdio.h>

typedef struct
{
  int (*imm)(int);
  int (*reg)(int);
} H;

__attribute__((noinline)) int fa(int x) { return x + 100; }
__attribute__((noinline)) int fb(int x) { return x + 200; }
__attribute__((noinline)) int fc(int x) { return x + 300; }
__attribute__((noinline)) int fd(int x) { return x + 400; }

static void two_halves(int *out, int n, H regular, H carry_h, int uses)
{
  for (int half = 0; half < 2; half++)
  {
    const H *h = (half == 0) ? &regular : &carry_h;
    if (uses && n > 3)
      out[half] = h->imm(n + half);
    else
      out[half] = h->reg(n - half);
  }
}

__attribute__((noinline)) void pick(int op, int *out, int n, int wide)
{
  H handler, carry;
  int uses = 0;
  switch (op)
  {
  case 0:
    handler.imm = fa;
    handler.reg = fb;
    carry.imm = fc;
    carry.reg = fd;
    uses = 1;
    break;
  case 1:
    handler.imm = fc;
    handler.reg = fd;
    carry = handler;
    break;
  case 2:
    handler.imm = fb;
    handler.reg = fa;
    carry = handler;
    break;
  default:
    return;
  }
  if (wide)
  {
    two_halves(out, n, handler, carry, uses);
    return;
  }
  out[0] = handler.reg(n);
  out[1] = -1;
}

int main(void)
{
  int out[2];
  for (int op = 0; op < 3; op++)
    for (int n = 2; n <= 5; n += 3)
    {
      pick(op, out, n, 1);
      printf("wide op=%d n=%d: %d %d\n", op, n, out[0], out[1]);
    }
  pick(0, out, 7, 0);
  printf("narrow op=0 n=7: %d %d\n", out[0], out[1]);
  return 0;
}
