/*
 * Bug: When a function returns a struct > 4 bytes (requiring a hidden
 * sret pointer in r0 per AAPCS), the ABI call layout for the callee's
 * explicit parameters was not advanced by one register. This caused the
 * last register parameter to be misclassified as in-register when it was
 * actually passed on the stack. The backend then generated ADD (compute
 * address) instead of LDR (load value), reading garbage.
 *
 * Reproducer: a function returning an 8-byte struct with 6 explicit
 * parameters. With sret consuming r0, r1-r3 hold the first 3 params
 * and the remaining 3 go on the stack.
 */
#include <stdio.h>

typedef struct
{
  unsigned char size;
  unsigned int opcode;
} Opcode;

static Opcode make_opcode(unsigned int op, unsigned int rd, unsigned int rn, unsigned int rm, int flags, int shift_val)
{
  /* Validate that none of the register values are garbage.
   * The bug caused rm (and subsequent stack params) to contain
   * the frame-pointer address instead of the actual value. */
  if (rd > 15 || rn > 15 || rm > 15)
  {
    printf("FAIL: invalid register rd=%u rn=%u rm=%u\n", rd, rn, rm);
    return (Opcode){0, 0};
  }

  unsigned int encoded = (op << 16) | (rn << 16) | (rd << 8) | rm | (shift_val << 4) | (flags << 20);
  return (Opcode){4, encoded};
}

int main(void)
{
  int fail = 0;

  /* Call with rm=5 (4th explicit arg -> goes on stack when sret uses r0) */
  Opcode o = make_opcode(0xea4f, 3, 15, 5, 0, 0);
  if (o.size != 4)
  {
    printf("FAIL: size=%u expected=4\n", (unsigned)o.size);
    fail = 1;
  }
  /* opcode should contain rm=5 in bits [3:0] */
  if ((o.opcode & 0xF) != 5)
  {
    printf("FAIL: rm bits=%u expected=5 (opcode=0x%08x)\n", o.opcode & 0xF, o.opcode);
    fail = 1;
  }
  /* rd=3 in bits [11:8] */
  if (((o.opcode >> 8) & 0xF) != 3)
  {
    printf("FAIL: rd bits=%u expected=3 (opcode=0x%08x)\n", (o.opcode >> 8) & 0xF, o.opcode);
    fail = 1;
  }

  /* Also test with different values to confirm stack params are read correctly */
  Opcode o2 = make_opcode(0xeb00, 7, 8, 12, 1, 2);
  if (o2.size != 4)
  {
    printf("FAIL: o2.size=%u expected=4\n", (unsigned)o2.size);
    fail = 1;
  }
  if ((o2.opcode & 0xF) != 12)
  {
    printf("FAIL: o2 rm bits=%u expected=12 (opcode=0x%08x)\n", o2.opcode & 0xF, o2.opcode);
    fail = 1;
  }

  if (!fail)
    printf("PASS\n");
  return fail;
}
