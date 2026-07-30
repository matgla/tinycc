/* Guard: ssa:global_addr_hoist must not rewrite STRUCT-typed symref operands.
 *
 * A by-value struct argument sourced from a const global
 * (FUNCPARAMVAL[SYMREF, btype=STRUCT]) keeps its type identity in the split
 * u.s encoding (ctype_idx).  Hoisting the global's address into a vreg and
 * rewriting the operand dropped ctype_idx, so the callsite marshaled the
 * struct with a garbage type: arm-thumb-gen.c's 12-byte THUMB_SHIFT_DEFAULT
 * was copied as a 32-byte blob with the wrong register split, th_mov_reg
 * received garbage, and the on-device tcc died with "received invalid
 * opcode: 0x0" on most -O2 compiles (48 suite tests).
 *
 * Several by-value uses of the same global push the class over the pass's
 * reload_estimate threshold so the hoist actually fires.
 */

#include <stdio.h>

typedef struct Sh { int t; unsigned v; int m; } Sh;

static const Sh DEF = {5, 60000, 7};

int sink;

int consume(unsigned rd, unsigned rm, int flags, Sh sh, int e, int b)
{
  sink += (int)(sh.t * 100 + (int)sh.v + sh.m * 10 + e + b + (int)rd + (int)rm + flags);
  return sh.t + sh.m;
}

volatile int vr;

int drive(void)
{
  int acc = 0;
  acc += consume(1u, 2u, 3, DEF, 4, 5);
  acc += consume((unsigned)vr, 6u, 7, DEF, 8, 9);
  acc += consume(10u, (unsigned)vr, 11, DEF, 12, 13);
  acc += consume(14u, 15u, vr, DEF, 16, 17);
  return acc;
}

int main(void)
{
  vr = 0;
  int acc = drive();
  printf("acc=%d sink=%d\n", acc, sink);
  return 0;
}
