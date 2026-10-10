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
#include "thop_mem_imm.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Block data transfer: PUSH, POP, LDM, STM, LDMDB, STMDB
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── PUSH ───── */

TH_TABLE(TH_PUSH, "push", {THOP_SHAPE_PUSH_T1, 0xb400, 0}, {THOP_SHAPE_PUSH_T2, 0xe92d0000, 0});

/* ───── POP ───── */

TH_TABLE(TH_POP, "pop", {THOP_SHAPE_POP_T1, 0xbc00, 0}, {THOP_SHAPE_POP_T2, 0xe8bd0000, 0});

/* ───── LDM ───── */

TH_TABLE(TH_LDM, "ldm", {THOP_SHAPE_LDM_T1, 0xc800, 0}, {THOP_SHAPE_LDM_T3, 0xe8900000, 0});

/* ───── STM ───── */

TH_TABLE(TH_STM, "stm", {THOP_SHAPE_STM_T1, 0xc000, 0}, {THOP_SHAPE_STM_T3, 0xe8800000, 0});

/* ───── LDMDB (T32) ───── */

TH_TABLE(TH_LDMDB, "ldmdb", {THOP_SHAPE_LDMDB, 0xe9100000, 0});

/* ───── STMDB (T32) ───── */

TH_TABLE(TH_STMDB, "stmdb", {THOP_SHAPE_STMDB, 0xe9000000, 0});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_push(uint32_t regs)
{
    uint8_t lr = (regs >> R_LR) & 1;
    regs &= ~((1u << R_LR) | (1u << R_PC));
    /* PUSH T2 (STMDB) with a single register is CONSTRAINED UNPREDICTABLE;
     * a lone high register uses PUSH T3: STR Rt, [SP, #-4]! (same 4 bytes). */
    if (regs != 0 && (regs & (regs - 1)) == 0 && !lr && (regs & ~0xffu))
        return th_str_imm((uint32_t)__builtin_ctz(regs), R_SP, 4, 5 /* pre-indexed, sub, writeback */,
                          ENFORCE_ENCODING_32BIT);
    return thop_emit_table(&TH_PUSH, (thop_args){.rm = regs, .imm = lr});
}

thumb_opcode th_pop(uint16_t regs)
{
    uint8_t pc = (regs >> R_PC) & 1;
    regs &= ~(1u << R_PC);
    /* POP T2 (LDMIA.W) with a single register is CONSTRAINED UNPREDICTABLE;
     * a lone high register or LR uses POP T3: LDR Rt, [SP], #4. */
    if (!pc && regs != 0 && (regs & (regs - 1)) == 0 && (regs & ~0xffu))
        return th_ldr_imm((uint32_t)__builtin_ctz(regs), R_SP, 4, 3 /* post-indexed, add, writeback */,
                          ENFORCE_ENCODING_32BIT);
    return thop_emit_table(&TH_POP, (thop_args){.rm = regs, .imm = pc});
}

static const thumb_opcode th_block_invalid = {.size = 0, .opcode = 0};

thumb_opcode th_ldm(uint32_t rn, uint32_t regset, uint32_t writeback, thumb_enforce_encoding encoding)
{
    if (rn == R_SP && writeback && encoding != ENFORCE_ENCODING_32BIT)
        return th_pop(regset);
    const uint32_t in_list = (regset >> rn) & 1u;
    /* Write-back with the base in the list is UNPREDICTABLE: reject. */
    if (writeback && in_list)
        return th_block_invalid;
    /* LDM T1 (16-bit) encodes write-back implicitly: it writes back exactly
     * when the base is NOT in the list. */
    if ((!writeback) != (in_list != 0))
    {
        if (encoding == ENFORCE_ENCODING_16BIT)
            return th_block_invalid;
        encoding = ENFORCE_ENCODING_32BIT;
    }
    return thop_emit_table(&TH_LDM, (thop_args){.rd = rn, .rm = regset, .imm = writeback, .enc = encoding});
}

thumb_opcode th_stm(uint32_t rn, uint32_t regset, uint32_t writeback, thumb_enforce_encoding encoding)
{
    const uint32_t in_list = (regset >> rn) & 1u;
    if (!writeback)
    {
        /* Only the 32-bit form exists without write-back. */
        if (encoding == ENFORCE_ENCODING_16BIT)
            return th_block_invalid;
        encoding = ENFORCE_ENCODING_32BIT;
    }
    else if (in_list)
    {
        /* Write-back with the base in the list: only STM T1 and only when the
         * base is the lowest register (it stores its original value).  The
         * 32-bit form is UNPREDICTABLE. */
        const bool t16_ok = rn <= 7 && (regset & ~0xffu) == 0 && (regset & ((1u << rn) - 1u)) == 0;
        if (!t16_ok || encoding == ENFORCE_ENCODING_32BIT)
            return th_block_invalid;
    }
    return thop_emit_table(&TH_STM, (thop_args){.rd = rn, .rm = regset, .imm = writeback, .enc = encoding});
}

thumb_opcode th_ldmdb(uint32_t rn, uint32_t reglist, uint32_t w)
{
    return thop_emit_table(&TH_LDMDB, (thop_args){.rd = rn, .rm = reglist, .imm = w});
}

thumb_opcode th_stmdb(uint32_t rn, uint32_t reglist, uint32_t w, thumb_enforce_encoding encoding)
{
    (void)encoding;
    return thop_emit_table(&TH_STMDB, (thop_args){.rd = rn, .rm = reglist, .imm = w});
}
