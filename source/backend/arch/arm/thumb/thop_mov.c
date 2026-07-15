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

/* T1 high-register MOV: MOV <Rd>, <Rm>  —  no shift, no S */
static const thop_variant_shape SHAPE_MOV_REG_T1_HIGH = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .dn_rd_split = {0, 3},
    .rm_place = {3, 4},
    .rd_con = REG_NOT_PC,
    .rm_con = REG_ANY,
    .feat = {.t16 = 1},
};

/* T1 shift alias: LSL/LSR/ASR <Rd>, <Rm>, #imm  —  low regs, implicit S */
static thumb_opcode mov_reg_t1_shift_emit(uint32_t base, const thop_args *a)
{
  (void)base;
  if (a->rd < 8 && a->rm < 8 && a->shift.type != THUMB_SHIFT_RRX && a->shift.type != THUMB_SHIFT_ROR &&
      ((a->flags == FLAGS_BEHAVIOUR_SET && !a->in_it_block) || (a->flags != FLAGS_BEHAVIOUR_SET && a->in_it_block)))
  {
    THOP_TRACE("%s %s, %s, #%u\n", th_shift_name(a->shift.type), th_reg_name(a->rd), th_reg_name(a->rm),
               (unsigned)a->shift.value);
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x0000 | (th_shift_value_to_sr_type(a->shift) << 11) | (a->shift.value << 6) | (a->rm << 3) | a->rd),
    };
  }
  return (thumb_opcode){.size = 0, .opcode = 0};
}

static const thop_variant_shape SHAPE_MOV_REG_T1_SHIFT = {
    .size = THOP_VARIANT_T16,
    .shift_allowed = (1u << THUMB_SHIFT_LSL) | (1u << THUMB_SHIFT_LSR) | (1u << THUMB_SHIFT_ASR),
    .has_s_bit = 1,
    .feat = {.t16 = 1},
};

/* T3 wide MOV: MOV{S}.W <Rd>, <Rm>{,shift} */
static const thop_variant_shape SHAPE_MOV_REG_T3 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rm_place = {0, 4},
    .has_s_bit = 1,
    .shift_type_bits = {4, 2},
    .shift_imm2_bits = {6, 2},
    .shift_imm3_bits = {12, 3},
    .shift_allowed = (1u << THUMB_SHIFT_LSL) | (1u << THUMB_SHIFT_LSR) | (1u << THUMB_SHIFT_ASR) |
                     (1u << THUMB_SHIFT_ROR) | (1u << THUMB_SHIFT_RRX),
    .feat = {.t32 = 1},
};

TH_TABLE(TH_MOV_REG, "mov",
          {&SHAPE_MOV_REG_T1_HIGH, 0x4600},
          {&SHAPE_MOV_REG_T1_SHIFT, 0, mov_reg_t1_shift_emit},
          {&SHAPE_MOV_REG_T3, 0xea4f0000});

thumb_opcode th_mov_reg(uint32_t rd, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding, bool in_it)
{
  if (shift.mode == THUMB_SHIFT_REGISTER && shift.type != THUMB_SHIFT_NONE)
    return th_mov_reg_shift(rd, rm, shift.value, flags, shift, encoding);

  return thop_emit(TH_MOV_REG.name, TH_MOV_REG.variants, TH_MOV_REG.variant_count,
                   (thop_args){.rd = rd, .rm = rm, .flags = flags, .shift = shift, .enc = encoding, .in_it_block = in_it});
}

/* ───── MOV immediate ───── */

/* T1: MOVS <Rd>, #<imm8>  —  low regs, implicit S, no BLOCK flags */
static thumb_opcode mov_imm_t1_emit(uint32_t base, const thop_args *a)
{
  if (a->rd <= 7 && a->imm <= 255 && a->flags != FLAGS_BEHAVIOUR_BLOCK)
  {
    THOP_TRACE("movs %s, #%u\n", th_reg_name(a->rd), (unsigned)a->imm);
    return (thumb_opcode){.size = 2, .opcode = base | (a->rd << 8) | a->imm};
  }
  return (thumb_opcode){.size = 0, .opcode = 0};
}

static const thop_variant_shape SHAPE_MOV_IMM_T1 = {
    .size = THOP_VARIANT_T16,
    .implicit_s = true,
    .feat = {.t16 = 1},
};

/* T3: MOV <Rd>, #<const>  —  modified immediate */
static const thop_variant_shape SHAPE_MOV_IMM_T3 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_NOT_SP | REG_NOT_PC,
    .imm = {.kind = IMM_PACK_CONST},
    .has_s_bit = 1,
    .feat = {.t32 = 1, .mod_imm = 1},
};

/* T4: MOVW <Rd>, #<imm16> */
static const thop_variant_shape SHAPE_MOV_IMM_T4 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_NOT_SP | REG_NOT_PC,
    .imm = {.kind = IMM_PACK_3_8_1},
    .feat = {.t32 = 1, .movw_movt = 1},
};

TH_TABLE(TH_MOV_IMM, "mov",
          {&SHAPE_MOV_IMM_T1, 0x2000, mov_imm_t1_emit},
          {&SHAPE_MOV_IMM_T3, 0xf04f0000},
          {&SHAPE_MOV_IMM_T4, 0xf2400000});

thumb_opcode th_mov_imm(uint16_t rd, uint32_t imm, thumb_flags_behaviour setflags, thumb_enforce_encoding encoding)
{
  return thop_emit(TH_MOV_IMM.name, TH_MOV_IMM.variants, TH_MOV_IMM.variant_count,
                   (thop_args){.rd = rd, .imm = imm, .flags = setflags, .enc = encoding});
}

/* ───── MOVT ───── */

static const thop_variant_shape SHAPE_MOVT = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_NOT_SP | REG_NOT_PC,
    .imm = {.kind = IMM_PACK_3_8_1},
    .feat = {.t32 = 1, .movw_movt = 1},
};

TH_TABLE(TH_MOVT, "movt", {&SHAPE_MOVT, 0xf2c00000});

thumb_opcode th_movt(uint32_t rd, uint32_t imm16)
{
  return thop_emit(TH_MOVT.name, TH_MOVT.variants, TH_MOVT.variant_count,
                   (thop_args){.rd = rd, .imm = imm16});
}

/* ───── MOV register-controlled shift ───── */

/* T1: MOV <Rd>, <Rm>, <shift> <Rs> — low regs, rd==rm */
static thumb_opcode mov_reg_shift_t1_emit(uint32_t base, const thop_args *a)
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

static const thop_variant_shape SHAPE_MOV_REG_SHIFT_T1 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rm_place = {0, 3},
    .ra_place = {3, 3},
    .rd_con = REG_LOW_ONLY | REG_EQ_RM,
    .rm_con = REG_LOW_ONLY,
    .ra_con = REG_LOW_ONLY,
    .implicit_s = 1,
    .shift_allowed = (1u << THUMB_SHIFT_LSL) | (1u << THUMB_SHIFT_LSR) | (1u << THUMB_SHIFT_ASR) |
                     (1u << THUMB_SHIFT_ROR),
    .feat = {.t16 = 1},
};

static const thop_variant_shape SHAPE_MOV_REG_SHIFT_T3 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rm_place = {16, 4},
    .ra_place = {0, 4},
    .has_s_bit = 1,
    .shift_type_bits = {21, 2},
    .shift_allowed = (1u << THUMB_SHIFT_LSL) | (1u << THUMB_SHIFT_LSR) | (1u << THUMB_SHIFT_ASR) |
                     (1u << THUMB_SHIFT_ROR),
    .feat = {.t32 = 1},
};

TH_TABLE(TH_MOV_REG_SHIFT, "mov",
         {&SHAPE_MOV_REG_SHIFT_T1, 0, mov_reg_shift_t1_emit},
         {&SHAPE_MOV_REG_SHIFT_T3, 0xfa00f000});

thumb_opcode th_mov_reg_shift(uint32_t rd, uint32_t rm, uint32_t rs, thumb_flags_behaviour flags, thumb_shift shift,
                              thumb_enforce_encoding encoding)
{
  return thop_emit(TH_MOV_REG_SHIFT.name, TH_MOV_REG_SHIFT.variants, TH_MOV_REG_SHIFT.variant_count,
                   (thop_args){.rd = rd, .rm = rm, .ra = rs, .flags = flags, .shift = shift, .enc = encoding});
}
