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

thumb_opcode th_ldrt(uint32_t rt, uint32_t rn, int imm);
thumb_opcode th_ldrbt(uint32_t rt, uint32_t rn, int imm);
thumb_opcode th_ldrht(uint32_t rt, uint32_t rn, int imm);
thumb_opcode th_ldrsbt(uint32_t rt, uint32_t rn, int imm);
thumb_opcode th_ldrsht(uint32_t rt, uint32_t rn, int imm);
thumb_opcode th_strt(uint32_t rt, uint32_t rn, int imm);
thumb_opcode th_strbt(uint32_t rt, uint32_t rn, int imm);
thumb_opcode th_strht(uint32_t rt, uint32_t rn, int imm);
