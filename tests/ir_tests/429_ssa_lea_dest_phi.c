/* Guard: SSA phi placement must see a LEA's DEST as a fresh definition.
 *
 * ssa_scan_var_defs skipped the whole instruction once ssa_mark_addrtaken
 * recognized a LEA (it marks the SOURCE var addrtaken), so a pointer var
 * defined BY the LEA never had that def recorded in def_blocks.  The rename
 * phase hands a LEA dest a fresh SSA name through the generic dest path, so
 * placement and rename disagreed: no phi was placed at the join, the use
 * bound to the older name, and
 *
 *     Op *base_op = 0;                    // def 1: #0
 *     if (cond(i)) { base_op = &src1; shl_idx = i; }   // def 2: LEA (invisible)
 *     if (shl_idx < 0) continue;
 *     divs[n].base_op = *base_op;         // use folded to *(Op *)0
 *
 * compiled to memmove(dst, NULL, 9) — at every -O level.  Found via the
 * on-device tcc: its find_derived_ivs (iv_analysis.c:364) HardFaulted on any
 * -O2 compile whose loop had an IV strength-reduction DIV candidate.
 */

#include <stdio.h>

typedef struct __attribute__((packed)) Op {
  unsigned int w0, w1;
  unsigned char b;
} Op;

typedef struct Div { int iv; Op base_op; } Div;

Div divs[4];
int num_divs = 0;
int hits[6] = {0, 0, 1, 0, 1, 0};

/* External linkage + global table: keeps the branch a real runtime branch. */
int cond(int i) { return hits[i]; }

Op g = {0x11223344u, 0x55667788u, 0x9a};

void find(int n)
{
  for (int i = 0; i < n; i++) {
    Op src1 = g;
    src1.w0 += (unsigned)i;
    Op *base_op = 0;
    int shl_idx = -1;
    if (cond(i)) { base_op = &src1; shl_idx = i; }
    if (shl_idx < 0) continue;
    divs[num_divs].iv = shl_idx;
    divs[num_divs].base_op = *base_op;
    num_divs++;
  }
}

/* Same shape, scalar member read through the pointer (no memmove involved). */
unsigned pick(int n)
{
  unsigned acc = 0;
  for (int i = 0; i < n; i++) {
    unsigned v = g.w1 + (unsigned)i;
    unsigned *p = 0;
    int found = -1;
    if (cond(i)) { p = &v; found = i; }
    if (found < 0) continue;
    acc += *p;
  }
  return acc;
}

int main(void)
{
  find(6);
  printf("num_divs=%d\n", num_divs);
  for (int k = 0; k < num_divs; k++)
    printf("div %d: iv=%d w0=%08x w1=%08x b=%02x\n", k, divs[k].iv,
           divs[k].base_op.w0, divs[k].base_op.w1, divs[k].base_op.b);
  printf("acc=%08x\n", pick(6));
  return 0;
}
