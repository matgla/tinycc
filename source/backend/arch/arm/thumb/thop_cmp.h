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

#pragma once

#include <stdint.h>

#include "thumb.h"

/* ───── Compare / Test immediate ─────
 *
 *  CMP/CMN/TST/TEQ only update flags; 32-bit forms hard-code Rd=0xF.
 */
thumb_opcode th_cmp_imm(uint32_t rn, uint32_t imm, thumb_flags_behaviour flags, thumb_enforce_encoding encoding);

/* Wrapper matching thumb_imm_handler_t for generic handler tables */
static inline thumb_opcode th_cmp_imm_handler(uint32_t rd, uint32_t rn, uint32_t imm,
                                              thumb_flags_behaviour flags, thumb_enforce_encoding enc)
{
    (void)rd;
    return th_cmp_imm(rn, imm, flags, enc);
}

thumb_opcode th_cmn_imm(uint32_t rn, uint32_t imm, thumb_flags_behaviour flags, thumb_enforce_encoding encoding);

thumb_opcode th_tst_imm(uint32_t rn, uint32_t imm, thumb_flags_behaviour flags, thumb_enforce_encoding encoding);

thumb_opcode th_teq_imm(uint32_t rn, uint32_t imm, thumb_flags_behaviour flags, thumb_enforce_encoding encoding);

/* ───── Compare / Test register ───── */
thumb_opcode th_cmp_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding);
thumb_opcode th_cmn_reg(uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding);
thumb_opcode th_tst_reg(uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding);
thumb_opcode th_teq_reg(uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding);
