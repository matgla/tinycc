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

#include "thop_coproc.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Coprocessor emit helpers
 *
 *  thop_args carries the operands positionally, since the coprocessor
 *  field layout (coproc / opc1 / opc2 / CRn / CRd / CRm) does not map
 *  onto the generic shape machinery:
 *
 *    rd   = CRd (cdp) or Rt   (mcr/mrc/mcrr/mrrc)
 *    rn   = CRn (cdp/mcr/mrc) or Rt2 (mcrr/mrrc)
 *    rm   = CRm
 *    ra   = opc1
 *    imm  = opc2
 *    imm2 = coproc number
 * ═══════════════════════════════════════════════════════════════════ */

/* CDP{2}: 1110 1110 opc1(4) CRn(4) | CRd(4) coproc(4) opc2(3) 0 CRm(4) */
static thumb_opcode coproc_cdp_emit(uint32_t base, const thop_args *a)
{
  uint32_t op = base | ((a->ra & 0xf) << 20) | ((a->rn & 0xf) << 16) | ((a->rd & 0xf) << 12) |
                ((a->imm2 & 0xf) << 8) | ((a->imm & 0x7) << 5) | (a->rm & 0xf);
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* MCR{2}/MRC{2}: 1110 111x opc1(3) L CRn(4) | Rt(4) coproc(4) opc2(3) 1 CRm(4)
 * The L bit (bit 20) lives in the base, so it is not set here. */
static thumb_opcode coproc_mcr_emit(uint32_t base, const thop_args *a)
{
  uint32_t op = base | ((a->ra & 0x7) << 21) | ((a->rn & 0xf) << 16) | ((a->rd & 0xf) << 12) |
                ((a->imm2 & 0xf) << 8) | ((a->imm & 0x7) << 5) | (a->rm & 0xf);
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* MCRR{2}/MRRC{2}: 1110 1100 010L Rt2(4) | Rt(4) coproc(4) opc1(4) CRm(4) */
static thumb_opcode coproc_mcrr_emit(uint32_t base, const thop_args *a)
{
  uint32_t op = base | ((a->rn & 0xf) << 16) | ((a->rd & 0xf) << 12) | ((a->imm2 & 0xf) << 8) |
                ((a->ra & 0xf) << 4) | (a->rm & 0xf);
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* ═══════════════════════════════════════════════════════════════════
 *  Shared shape
 *
 *  No field placement and no register constraints: the custom emitters
 *  own the whole word, and CRn/CRd/CRm are coprocessor register numbers
 *  rather than core registers, so the generic reg_mask checks would be
 *  meaningless.  Rt == R_PC is legal on MRC (APSR_nzcv form).
 * ═══════════════════════════════════════════════════════════════════ */

static const thop_variant_shape SHAPE_COPROC = {
    .size = THOP_VARIANT_T32,
    .feat = {.t32 = 1, .coproc = 1},
};

/* ═══════════════════════════════════════════════════════════════════
 *  THOP tables
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_CDP, "cdp", {&SHAPE_COPROC, 0xee000000, coproc_cdp_emit});
TH_TABLE(TH_CDP2, "cdp2", {&SHAPE_COPROC, 0xfe000000, coproc_cdp_emit});

TH_TABLE(TH_MCR, "mcr", {&SHAPE_COPROC, 0xee000010, coproc_mcr_emit});
TH_TABLE(TH_MCR2, "mcr2", {&SHAPE_COPROC, 0xfe000010, coproc_mcr_emit});

TH_TABLE(TH_MRC, "mrc", {&SHAPE_COPROC, 0xee100010, coproc_mcr_emit});
TH_TABLE(TH_MRC2, "mrc2", {&SHAPE_COPROC, 0xfe100010, coproc_mcr_emit});

TH_TABLE(TH_MCRR, "mcrr", {&SHAPE_COPROC, 0xec400000, coproc_mcrr_emit});
TH_TABLE(TH_MCRR2, "mcrr2", {&SHAPE_COPROC, 0xfc400000, coproc_mcrr_emit});

TH_TABLE(TH_MRRC, "mrrc", {&SHAPE_COPROC, 0xec500000, coproc_mcrr_emit});
TH_TABLE(TH_MRRC2, "mrrc2", {&SHAPE_COPROC, 0xfc500000, coproc_mcrr_emit});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_cdp(uint32_t coproc, uint32_t opc1, uint32_t crd, uint32_t crn, uint32_t crm, uint32_t opc2,
                    uint32_t two)
{
  const thop_table *t = two ? &TH_CDP2 : &TH_CDP;
  return thop_emit(t->name, t->variants, t->variant_count,
                   (thop_args){.rd = crd, .rn = crn, .rm = crm, .ra = opc1, .imm = opc2, .imm2 = coproc});
}

thumb_opcode th_mcr(uint32_t coproc, uint32_t opc1, uint32_t rt, uint32_t crn, uint32_t crm, uint32_t opc2,
                    uint32_t two)
{
  const thop_table *t = two ? &TH_MCR2 : &TH_MCR;
  return thop_emit(t->name, t->variants, t->variant_count,
                   (thop_args){.rd = rt, .rn = crn, .rm = crm, .ra = opc1, .imm = opc2, .imm2 = coproc});
}

thumb_opcode th_mrc(uint32_t coproc, uint32_t opc1, uint32_t rt, uint32_t crn, uint32_t crm, uint32_t opc2,
                    uint32_t two)
{
  const thop_table *t = two ? &TH_MRC2 : &TH_MRC;
  return thop_emit(t->name, t->variants, t->variant_count,
                   (thop_args){.rd = rt, .rn = crn, .rm = crm, .ra = opc1, .imm = opc2, .imm2 = coproc});
}

thumb_opcode th_mcrr(uint32_t coproc, uint32_t opc1, uint32_t rt, uint32_t rt2, uint32_t crm, uint32_t two)
{
  const thop_table *t = two ? &TH_MCRR2 : &TH_MCRR;
  return thop_emit(t->name, t->variants, t->variant_count,
                   (thop_args){.rd = rt, .rn = rt2, .rm = crm, .ra = opc1, .imm2 = coproc});
}

thumb_opcode th_mrrc(uint32_t coproc, uint32_t opc1, uint32_t rt, uint32_t rt2, uint32_t crm, uint32_t two)
{
  const thop_table *t = two ? &TH_MRRC2 : &TH_MRRC;
  return thop_emit(t->name, t->variants, t->variant_count,
                   (thop_args){.rd = rt, .rn = rt2, .rm = crm, .ra = opc1, .imm2 = coproc});
}
