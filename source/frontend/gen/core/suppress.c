/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* suppress.c -- Automagical code suppression: forward/backward jump emission and the nocode_wanted state machine.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Automagical code suppression */

/* Clear 'nocode_wanted' at forward label if it was used */
ST_FUNC void gsym(int t)
{
  if (t > 0) /* -1 = no chain, 0 = instruction 0 (but gsym is for machine code, not IR) */
  {
    gsym_addr(t, ind);
    CODE_ON();
  }
}

/* Forward declaration for nested function handling */

/* Clear 'nocode_wanted' if current pc is a label */
int gind()
{
  int t = tcc_state->ir->next_instruction_index;
  CODE_ON();
  if (debug_modes)
    tcc_tcov_block_begin(tcc_state);
  return t;
}

/* Set 'nocode_wanted' after unconditional (forwards) jump */
int gjmp_acs(int t)
{
  // t = gjmp(t);
  SValue dest;
  svalue_init(&dest);
  dest.vr = -1;
  dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
  dest.c.i = t;
  t = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);

  CODE_OFF();
  return t;
}

/* Jump to an already-emitted instruction (loop backedge, backward goto).
 * Unlike the gsym chain path, nothing later backpatches this jump, so the
 * target must be marked as a jump target HERE — optimization passes (e.g.
 * setif_branch_fuse) trust is_jump_target when deciding whether a flag-setting
 * instruction can be removed, and a fallthrough-only loop condition has no
 * other jump that would mark it. */
int gjmp_addr_acs(int a)
{
  TCCIRState *ir = tcc_state->ir;
  SValue dest;
  svalue_init(&dest);
  dest.vr = -1;
  dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
  dest.c.i = a;
  int t = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
  if (a >= 0 && a < ir->next_instruction_index)
    ir->compact_instructions[a].is_jump_target = 1;
  else if (a == ir->next_instruction_index)
    ir->next_insn_is_jump_target = 1;
  return t;
}

/* These are #undef'd at the end of this file */
