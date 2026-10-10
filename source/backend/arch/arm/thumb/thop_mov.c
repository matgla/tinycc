/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
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

#define USING_GLOBALS
#include "thop_mov.h"
#include "thumb.h"
#include "tcc.h"

/* ═══════════════════════════════════════════════════════════════════
 *  MOV — move (register, immediate, top-half)
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── MOV register ───── */

/* T1 shift alias: LSL/LSR/ASR <Rd>, <Rm>, #imm  —  low regs, implicit S */
thumb_opcode mov_reg_t1_shift_emit(uint32_t base, const thop_args *a)
{
  (void)base;
  if (a->rd < 8 && a->rm < 8 && a->shift.type != THUMB_SHIFT_RRX && a->shift.type != THUMB_SHIFT_ROR &&
      ((a->flags == FLAGS_BEHAVIOUR_SET && !a->in_it_block) || (a->flags != FLAGS_BEHAVIOUR_SET && a->in_it_block)))
  {
    THOP_TRACE("%s %s, %s, #%u\n", th_shift_name(a->shift.type), th_reg_name(a->rd), th_reg_name(a->rm),
               (unsigned)a->shift.value);
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x0000 | (th_shift_value_to_sr_type(a->shift) << 11) | ((a->shift.value & 0x1f) << 6) | (a->rm << 3) | a->rd),
    };
  }
  return (thumb_opcode){.size = 0, .opcode = 0};
}

TH_TABLE(TH_MOV_REG, "mov",
          {THOP_SHAPE_MOV_REG_T1_HIGH, 0x4600},
          {THOP_SHAPE_MOV_REG_T1_SHIFT, 0, THOP_CUSTOM_mov_reg_t1_shift_emit},
          {THOP_SHAPE_MOV_REG_T3, 0xea4f0000});

thumb_opcode th_mov_reg(uint32_t rd, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding, bool in_it)
{
  if (shift.mode == THUMB_SHIFT_REGISTER && shift.type != THUMB_SHIFT_NONE)
    return th_mov_reg_shift(rd, rm, shift.value, flags, shift, encoding);

  if (shift.mode == THUMB_SHIFT_IMMEDIATE && !th_shift_imm_normalize(&shift))
    return (thumb_opcode){.size = 0, .opcode = 0};

  return thop_emit_table(&TH_MOV_REG, (thop_args){.rd = rd, .rm = rm, .flags = flags, .shift = shift, .enc = encoding, .in_it_block = in_it});
}

/* ───── MOV immediate ───── */

/* T1: MOVS <Rd>, #<imm8>  —  low regs, implicit S, no BLOCK flags */
thumb_opcode mov_imm_t1_emit(uint32_t base, const thop_args *a)
{
  if (a->rd <= 7 && a->imm <= 255 && a->flags != FLAGS_BEHAVIOUR_BLOCK)
  {
    THOP_TRACE("movs %s, #%u\n", th_reg_name(a->rd), (unsigned)a->imm);
    return (thumb_opcode){.size = 2, .opcode = base | (a->rd << 8) | a->imm};
  }
  return (thumb_opcode){.size = 0, .opcode = 0};
}

TH_TABLE(TH_MOV_IMM, "mov",
          {THOP_SHAPE_MOV_IMM_T1, 0x2000, THOP_CUSTOM_mov_imm_t1_emit},
          {THOP_SHAPE_MOV_IMM_T3, 0xf04f0000},
          {THOP_SHAPE_MOV_IMM_T4, 0xf2400000});

thumb_opcode th_mov_imm(uint16_t rd, uint32_t imm, thumb_flags_behaviour setflags, thumb_enforce_encoding encoding)
{
  return thop_emit_table(&TH_MOV_IMM, (thop_args){.rd = rd, .imm = imm, .flags = setflags, .enc = encoding});
}

/* ───── MOVT ───── */

TH_TABLE(TH_MOVT, "movt", {THOP_SHAPE_MOVT, 0xf2c00000});

thumb_opcode th_movt(uint32_t rd, uint32_t imm16)
{
  return thop_emit_table(&TH_MOVT, (thop_args){.rd = rd, .imm = imm16});
}

/* ───── MOV register-controlled shift ───── */

/* T1: MOV <Rd>, <Rm>, <shift> <Rs> — low regs, rd==rm */
thumb_opcode mov_reg_shift_t1_emit(uint32_t base, const thop_args *a)
{
  (void)base;
  if (a->rd == a->rm && a->rd < 8 && a->ra < 8 && a->enc != ENFORCE_ENCODING_32BIT && a->shift.type != THUMB_SHIFT_RRX)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4000 | (a->ra << 3) | (th_shift_type_to_op(a->shift) << 6) | a->rd,
    };
  }
  return (thumb_opcode){.size = 0, .opcode = 0};
}

TH_TABLE(TH_MOV_REG_SHIFT, "mov",
         {THOP_SHAPE_MOV_REG_SHIFT_T1, 0, THOP_CUSTOM_mov_reg_shift_t1_emit},
         {THOP_SHAPE_MOV_REG_SHIFT_T3, 0xfa00f000});

thumb_opcode th_mov_reg_shift(uint32_t rd, uint32_t rm, uint32_t rs, thumb_flags_behaviour flags, thumb_shift shift,
                              thumb_enforce_encoding encoding)
{
  return thop_emit_table(&TH_MOV_REG_SHIFT, (thop_args){.rd = rd, .rm = rm, .ra = rs, .flags = flags, .shift = shift, .enc = encoding});
}
