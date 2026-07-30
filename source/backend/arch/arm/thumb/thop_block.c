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

#include "thop_block.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Block data transfer: PUSH, POP, LDM, STM, LDMDB, STMDB
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── PUSH ───── */

/* T1 narrow: push {reglist}, [lr]  —  raw reglist in bits [7:0], lr flag at bit 8 */
static const thop_variant_shape SHAPE_PUSH_T1 = {
    .size = THOP_VARIANT_T16,
    .rm_raw_place = {0, 8},           /* raw register list in bits [7:0] */
    .imm = {.kind = IMM_RAW, .width = 1},
    .imm_place = {8, 1},              /* LR flag at bit 8 */
    .rm_con = REG_LOW_REGSET | REG_RM_BITS_NOT_LR_PC,
    .feat = {.t16 = 1},
};

/* T2 wide: push {reglist}  —  register list in bits [15:3], SP/PC not allowed */
static const thop_variant_shape SHAPE_PUSH_T2 = {
    .size = THOP_VARIANT_T32,
    .rm_place = {0, 13},              /* register list in bits [12:0] (r0-r12) */
    .imm = {.kind = IMM_RAW, .width = 1},
    .imm_place = {14, 1},             /* LR/M flag at bit 14 */
    .rm_con = REG_RM_BITS_NOT_LR_PC,
    .feat = {.t32 = 1},
};

TH_TABLE(TH_PUSH, "push", {&SHAPE_PUSH_T1, 0xb400, NULL}, {&SHAPE_PUSH_T2, 0xe92d0000, NULL});

/* ───── POP ───── */

/* T1 narrow: pop {reglist}, [pc]  —  raw reglist in bits [7:0], pc flag at bit 8 */
static const thop_variant_shape SHAPE_POP_T1 = {
    .size = THOP_VARIANT_T16,
    .rm_raw_place = {0, 8},           /* raw register list in bits [7:0] */
    .imm = {.kind = IMM_RAW, .width = 1},
    .imm_place = {8, 1},              /* PC flag at bit 8 */
    .rm_con = REG_LOW_REGSET | REG_RM_BITS_NOT_LR_PC,
    .feat = {.t16 = 1},
};

/* T2 wide: pop {reglist}  —  register list in bits [15:3], SP not allowed */
static const thop_variant_shape SHAPE_POP_T2 = {
    .size = THOP_VARIANT_T32,
    .rm_place = {0, 15},              /* register list in bits [14:0] (r0-r12 + LR) */
    .imm = {.kind = IMM_RAW, .width = 1},
    .imm_place = {15, 1},             /* PC/P flag at bit 15 */
    .rm_con = REG_RM_BIT_NOT_SP,
    .feat = {.t32 = 1},
};

TH_TABLE(TH_POP, "pop", {&SHAPE_POP_T1, 0xbc00, NULL}, {&SHAPE_POP_T2, 0xe8bd0000, NULL});

/* ───── LDM ───── */

/* T1 narrow: ldm {rn}, {reglist}!  —  rn in bits [8:5], raw reglist in bits [7:0] */
static const thop_variant_shape SHAPE_LDM_T1 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {8, 3},               /* rn in bits [10:8] */
    .rd_con = REG_LOW_ONLY,
    .rm_raw_place = {0, 8},           /* raw register list in bits [7:0] */
    .rm_con = REG_LOW_REGSET,
    .feat = {.t16 = 1},
};

/* T3 wide: ldmia {rn}!, {reglist}  —  rn at [19:16], reglist at [12:0], writeback at [21] */
static const thop_variant_shape SHAPE_LDM_T3 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {16, 4},              /* rn in bits [19:16] */
    .rm_place = {0, 16},              /* register list in bits [15:0] (r0-r12, LR, PC) */
    .rm_con = REG_RM_BIT_NOT_SP,
    .imm = {.kind = IMM_RAW, .width = 1},
    .imm_place = {21, 1},             /* writeback bit at position 21 */
    .feat = {.t32 = 1},
};

TH_TABLE(TH_LDM, "ldm", {&SHAPE_LDM_T1, 0xc800, NULL}, {&SHAPE_LDM_T3, 0xe8900000, NULL});

/* ───── STM ───── */

/* T1 narrow: stm {rn}!, {reglist}  —  rn in bits [8:5], raw reglist in bits [7:0] */
static const thop_variant_shape SHAPE_STM_T1 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {8, 3},               /* rn in bits [10:8] */
    .rd_con = REG_LOW_ONLY,
    .rm_raw_place = {0, 8},           /* raw register list in bits [7:0] */
    .rm_con = REG_LOW_REGSET,
    .feat = {.t16 = 1},
};

/* T3 wide: stmia {rn}!, {reglist}  —  rn at [19:16], reglist at [12:0], writeback at [21] */
static const thop_variant_shape SHAPE_STM_T3 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {16, 4},              /* rn in bits [19:16] */
    .rm_place = {0, 15},              /* register list in bits [14:0] (r0-r12, LR) */
    .rm_con = REG_RM_BIT_NOT_SP,
    .imm = {.kind = IMM_RAW, .width = 1},
    .imm_place = {21, 1},             /* writeback bit at position 21 */
    .feat = {.t32 = 1},
};

TH_TABLE(TH_STM, "stm", {&SHAPE_STM_T1, 0xc000, NULL}, {&SHAPE_STM_T3, 0xe8800000, NULL});

/* ───── LDMDB (T32) ───── */

static const thop_variant_shape SHAPE_LDMDB = {
    .size = THOP_VARIANT_T32,
    .rd_place = {16, 4},
    .rm_place = {0, 16},              /* register list in bits [15:0] (r0-r12, LR, PC) */
    .rm_con = REG_RM_BIT_NOT_SP,
    .imm = {.kind = IMM_RAW, .width = 1},
    .imm_place = {21, 1},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_LDMDB, "ldmdb", {&SHAPE_LDMDB, 0xe9100000, NULL});

/* ───── STMDB (T32) ───── */

static const thop_variant_shape SHAPE_STMDB = {
    .size = THOP_VARIANT_T32,
    .rd_place = {16, 4},
    .rm_place = {0, 15},              /* register list in bits [14:0] (r0-r12, LR) */
    .rm_con = REG_RM_BIT_NOT_SP,
    .imm = {.kind = IMM_RAW, .width = 1},
    .imm_place = {21, 1},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_STMDB, "stmdb", {&SHAPE_STMDB, 0xe9000000, NULL});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_push(uint32_t regs)
{
    uint8_t lr = (regs >> R_LR) & 1;
    regs &= ~((1u << R_LR) | (1u << R_PC));
    return thop_emit(TH_PUSH.name, TH_PUSH.variants, TH_PUSH.variant_count,
                     (thop_args){.rm = regs, .imm = lr});
}

thumb_opcode th_pop(uint16_t regs)
{
    uint8_t pc = (regs >> R_PC) & 1;
    regs &= ~(1u << R_PC);
    return thop_emit(TH_POP.name, TH_POP.variants, TH_POP.variant_count,
                     (thop_args){.rm = regs, .imm = pc});
}

thumb_opcode th_ldm(uint32_t rn, uint32_t regset, uint32_t writeback, thumb_enforce_encoding encoding)
{
    if (rn == R_SP && writeback && encoding != ENFORCE_ENCODING_32BIT)
        return th_pop(regset);
    if (!writeback)
        encoding = ENFORCE_ENCODING_32BIT;
    regset &= ~(1u << rn);
    return thop_emit(TH_LDM.name, TH_LDM.variants, TH_LDM.variant_count,
                     (thop_args){.rd = rn, .rm = regset, .imm = writeback, .enc = encoding});
}

thumb_opcode th_stm(uint32_t rn, uint32_t regset, uint32_t writeback, thumb_enforce_encoding encoding)
{
    if (!writeback)
        encoding = ENFORCE_ENCODING_32BIT;
    regset &= ~(1u << rn);
    return thop_emit(TH_STM.name, TH_STM.variants, TH_STM.variant_count,
                     (thop_args){.rd = rn, .rm = regset, .imm = writeback, .enc = encoding});
}

thumb_opcode th_ldmdb(uint32_t rn, uint32_t reglist, uint32_t w)
{
    return thop_emit(TH_LDMDB.name, TH_LDMDB.variants, TH_LDMDB.variant_count,
                     (thop_args){.rd = rn, .rm = reglist, .imm = w});
}

thumb_opcode th_stmdb(uint32_t rn, uint32_t reglist, uint32_t w, thumb_enforce_encoding encoding)
{
    (void)encoding;
    return thop_emit(TH_STMDB.name, TH_STMDB.variants, TH_STMDB.variant_count,
                     (thop_args){.rd = rn, .rm = reglist, .imm = w});
}
