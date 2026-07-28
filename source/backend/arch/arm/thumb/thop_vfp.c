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

#include "thop_vfp.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  VFP custom emit helpers — handle D:Vd / N:Vn / M:Vm split encoding
 * ═══════════════════════════════════════════════════════════════════ */

static void vfp_pack_sp(uint32_t reg, uint32_t *D, uint32_t *V)
{
  *D = reg & 1;
  *V = (reg >> 1) & 0xf;
}

static void vfp_pack_dp(uint32_t reg, uint32_t *D, uint32_t *V)
{
  *D = (reg >> 4) & 1;
  *V = reg & 0xf;
}

/* 3-register arithmetic (vadd, vsub, vmul, vdiv) */
static thumb_opcode vfp_arith3_emit(uint32_t base, const thop_args *a)
{
  uint32_t D, Vd, N, Vn, M, Vm;
  if (base & (1u << 8))
  {
    vfp_pack_dp(a->rd, &D, &Vd);
    vfp_pack_dp(a->rn, &N, &Vn);
    vfp_pack_dp(a->rm, &M, &Vm);
  }
  else
  {
    vfp_pack_sp(a->rd, &D, &Vd);
    vfp_pack_sp(a->rn, &N, &Vn);
    vfp_pack_sp(a->rm, &M, &Vm);
  }
  uint32_t op = base | (D << 22) | (Vn << 16) | (Vd << 12) | (N << 7) | (M << 5) | Vm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* 2-register arithmetic / compare (vneg, vcmp, vcmpe) */
static thumb_opcode vfp_arith2_emit(uint32_t base, const thop_args *a)
{
  uint32_t D, Vd, M, Vm;
  if (base & (1u << 8))
  {
    vfp_pack_dp(a->rd, &D, &Vd);
    vfp_pack_dp(a->rm, &M, &Vm);
  }
  else
  {
    vfp_pack_sp(a->rd, &D, &Vd);
    vfp_pack_sp(a->rm, &M, &Vm);
  }
  uint32_t op = base | (D << 22) | (Vd << 12) | (M << 5) | Vm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* Register move (vmov_register) */
static thumb_opcode vmov_reg_emit(uint32_t base, const thop_args *a)
{
  uint32_t D, Vd, M, Vm;
  if (base & (1u << 8))
  {
    vfp_pack_dp(a->rd, &D, &Vd);
    vfp_pack_dp(a->rm, &M, &Vm);
  }
  else
  {
    vfp_pack_sp(a->rd, &D, &Vd);
    vfp_pack_sp(a->rm, &M, &Vm);
  }
  uint32_t op = base | (D << 22) | (Vd << 12) | (M << 5) | Vm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* Push / pop (vpush, vpop) */
static thumb_opcode vfp_pushpop_emit(uint32_t base, const thop_args *a)
{
  uint32_t regs = a->imm;
  uint32_t is_doubleword = (base >> 8) & 1;

  int first_register = 0;
  int register_count = 0;
  for (int i = 0; i < 32; i++)
  {
    if (regs & (1u << i))
    {
      first_register = i;
      break;
    }
  }
  for (int i = 0; i < 32; i++)
  {
    if (regs & (1u << i))
      register_count++;
  }

  uint32_t D, Vd;
  if (is_doubleword)
  {
    D = (first_register >> 4) & 1;
    Vd = first_register & 0xf;
    register_count <<= 1;
  }
  else
  {
    D = first_register & 1;
    Vd = (first_register >> 1) & 0xf;
  }

  uint32_t op = base | (D << 22) | (Vd << 12) | (register_count & 0xff);
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* VLDR / VSTR single-register load/store (SP coproc 1010 / DP coproc 1011).
 *   base carries the U=add bit set; a->imm2==0 selects a subtracted offset.
 *   a->imm is the pre-scaled imm8 (byte offset / 4), a->rn the GPR base. */
static thumb_opcode vfp_ldst_emit(uint32_t base, const thop_args *a)
{
  uint32_t D, Vd;
  if (base & (1u << 8))
    vfp_pack_dp(a->rd, &D, &Vd);
  else
    vfp_pack_sp(a->rd, &D, &Vd);
  uint32_t op = base | (D << 22) | (a->rn << 16) | (Vd << 12) | (a->imm & 0xff);
  if (a->imm2 == 0)
    op &= ~(1u << 23);
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* VMOV between GPR and single-precision VFP register */
static thumb_opcode vmov_gp_sp_emit(uint32_t base, const thop_args *a)
{
  uint32_t Vn = (a->rn >> 1) & 0xf;
  uint32_t N = a->rn & 1;
  uint32_t op = base | (a->imm2 << 20) | (a->rd << 12) | (Vn << 16) | (N << 7);
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* VMOV between two GPRs and double-precision VFP register */
static thumb_opcode vmov_2gp_dp_emit(uint32_t base, const thop_args *a)
{
  uint32_t M = (a->rm >> 4) & 1;
  uint32_t Vm = a->rm & 0xf;
  uint32_t op = base | (a->imm2 << 20) | (a->rn << 16) | (a->rd << 12) | (M << 5) | Vm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* VCVT float-to-double and double-to-float */
static thumb_opcode vcvt_fd_emit(uint32_t base, const thop_args *a)
{
  uint32_t D = (a->rd >> 4) & 1;
  uint32_t Vd = a->rd & 0xf;
  uint32_t M = a->rm & 1;
  uint32_t Vm = (a->rm >> 1) & 0xf;
  uint32_t op = base | (D << 22) | (Vd << 12) | (M << 5) | Vm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

static thumb_opcode vcvt_df_emit(uint32_t base, const thop_args *a)
{
  uint32_t D = a->rd & 1;
  uint32_t Vd = (a->rd >> 1) & 0xf;
  uint32_t M = (a->rm >> 4) & 1;
  uint32_t Vm = a->rm & 0xf;
  uint32_t op = base | (D << 22) | (Vd << 12) | (M << 5) | Vm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* VCVT between floating-point and integer */
static thumb_opcode vcvt_fp_int_emit(uint32_t base, const thop_args *a)
{
  uint32_t sz = (base >> 8) & 1;
  uint32_t is_fp_to_int = (a->imm != 0);
  uint32_t op_bit = is_fp_to_int | a->imm2;
  uint32_t D, Vd, M, Vm;

  if (is_fp_to_int)
  { /* fp -> int: destination is always Sd */
    vfp_pack_sp(a->rd, &D, &Vd);
  }
  else
  { /* int -> fp: destination is Sd (sz=0) or Dd (sz=1) */
    if (sz == 0)
      vfp_pack_sp(a->rd, &D, &Vd);
    else
      vfp_pack_dp(a->rd, &D, &Vd);
  }

  if (is_fp_to_int && sz == 1)
  { /* fp -> int with double source */
    vfp_pack_dp(a->rm, &M, &Vm);
  }
  else
  { /* source is Sm in all other cases */
    vfp_pack_sp(a->rm, &M, &Vm);
  }

  uint32_t op = base | (D << 22) | (Vd << 12) | (a->imm << 16) | (op_bit << 7) | 0x40 | (M << 5) | Vm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* ═══════════════════════════════════════════════════════════════════
 *  Shared shapes
 * ═══════════════════════════════════════════════════════════════════ */

static const thop_variant_shape SHAPE_VFP_SP = {
    .size = THOP_VARIANT_T32,
    .feat = {.t32 = 1, .vfp_sp = 1},
};

static const thop_variant_shape SHAPE_VFP_DP = {
    .size = THOP_VARIANT_T32,
    .feat = {.t32 = 1, .vfp_dp = 1},
};

static const thop_variant_shape SHAPE_VMOVGPSP = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4},
    .imm2_place = {20, 1},
    .feat = {.t32 = 1, .vfp_sp = 1},
};

static const thop_variant_shape SHAPE_VMOV2GPDP = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4},
    .rn_place = {16, 4},
    .imm2_place = {20, 1},
    .feat = {.t32 = 1, .vfp_dp = 1},
};

static const thop_variant_shape SHAPE_VMRS = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4},
    .feat = {.t32 = 1, .vfp_sp = 1},
};

static const thop_variant_shape SHAPE_VCVT_FD = {
    .size = THOP_VARIANT_T32,
    .feat = {.t32 = 1, .vfp_dp = 1},
};

static const thop_variant_shape SHAPE_VCVT_DF = {
    .size = THOP_VARIANT_T32,
    .feat = {.t32 = 1, .vfp_dp = 1},
};

static const thop_variant_shape SHAPE_VCVT_FP_INT_SP = {
    .size = THOP_VARIANT_T32,
    .imm = {.kind = IMM_RAW, .width = 4},
    .imm_place = {16, 4},
    .imm2_place = {7, 1},
    .puw_bits = {8, 1},
    .feat = {.t32 = 1, .vfp_sp = 1},
};

static const thop_variant_shape SHAPE_VCVT_FP_INT_DP = {
    .size = THOP_VARIANT_T32,
    .imm = {.kind = IMM_RAW, .width = 4},
    .imm_place = {16, 4},
    .imm2_place = {7, 1},
    .puw_bits = {8, 1},
    .feat = {.t32 = 1, .vfp_dp = 1},
};

/* ═══════════════════════════════════════════════════════════════════
 *  THOP tables
 * ═══════════════════════════════════════════════════════════════════ */

/* VADD.F32 / VADD.F64 */
TH_TABLE(TH_VADD_F_SP, "vadd.f32", {&SHAPE_VFP_SP, 0xee300a00, vfp_arith3_emit});
TH_TABLE(TH_VADD_F_DP, "vadd.f64", {&SHAPE_VFP_DP, 0xee300b00, vfp_arith3_emit});

/* VSUB.F32 / VSUB.F64 */
TH_TABLE(TH_VSUB_F_SP, "vsub.f32", {&SHAPE_VFP_SP, 0xee300a40, vfp_arith3_emit});
TH_TABLE(TH_VSUB_F_DP, "vsub.f64", {&SHAPE_VFP_DP, 0xee300b40, vfp_arith3_emit});

/* VMUL.F32 / VMUL.F64 */
TH_TABLE(TH_VMUL_F_SP, "vmul.f32", {&SHAPE_VFP_SP, 0xee200a00, vfp_arith3_emit});
TH_TABLE(TH_VMUL_F_DP, "vmul.f64", {&SHAPE_VFP_DP, 0xee200b00, vfp_arith3_emit});

/* VDIV.F32 / VDIV.F64 */
TH_TABLE(TH_VDIV_F_SP, "vdiv.f32", {&SHAPE_VFP_SP, 0xee800a00, vfp_arith3_emit});
TH_TABLE(TH_VDIV_F_DP, "vdiv.f64", {&SHAPE_VFP_DP, 0xee800b00, vfp_arith3_emit});

/* VNEG.F32 / VNEG.F64 */
TH_TABLE(TH_VNEG_F_SP, "vneg.f32", {&SHAPE_VFP_SP, 0xeeb10a40, vfp_arith2_emit});
TH_TABLE(TH_VNEG_F_DP, "vneg.f64", {&SHAPE_VFP_DP, 0xeeb10b40, vfp_arith2_emit});

/* VCMP.F32 / VCMP.F64 */
TH_TABLE(TH_VCMP_F_SP, "vcmp.f32", {&SHAPE_VFP_SP, 0xeeb40a40, vfp_arith2_emit});
TH_TABLE(TH_VCMP_F_DP, "vcmp.f64", {&SHAPE_VFP_DP, 0xeeb40b40, vfp_arith2_emit});

/* VPUSH SP / DP */
TH_TABLE(TH_VPUSH_SP, "vpush.f32", {&SHAPE_VFP_SP, 0xed2d0a00, vfp_pushpop_emit});
TH_TABLE(TH_VPUSH_DP, "vpush.f64", {&SHAPE_VFP_SP, 0xed2d0b00, vfp_pushpop_emit});

/* VPOP SP / DP */
TH_TABLE(TH_VPOP_SP, "vpop.f32", {&SHAPE_VFP_SP, 0xecbd0a00, vfp_pushpop_emit});
TH_TABLE(TH_VPOP_DP, "vpop.f64", {&SHAPE_VFP_SP, 0xecbd0b00, vfp_pushpop_emit});

/* VLDR SP / DP */
TH_TABLE(TH_VLDR_SP, "vldr.f32", {&SHAPE_VFP_SP, 0xed900a00, vfp_ldst_emit});
TH_TABLE(TH_VLDR_DP, "vldr.f64", {&SHAPE_VFP_DP, 0xed900b00, vfp_ldst_emit});

/* VSTR SP / DP */
TH_TABLE(TH_VSTR_SP, "vstr.f32", {&SHAPE_VFP_SP, 0xed800a00, vfp_ldst_emit});
TH_TABLE(TH_VSTR_DP, "vstr.f64", {&SHAPE_VFP_DP, 0xed800b00, vfp_ldst_emit});

/* VMOV register SP / DP */
TH_TABLE(TH_VMOV_REG_SP, "vmov.f32", {&SHAPE_VFP_SP, 0xeeb00a40, vmov_reg_emit});
TH_TABLE(TH_VMOV_REG_DP, "vmov.f64", {&SHAPE_VFP_DP, 0xeeb00b40, vmov_reg_emit});

/* VMOV between GPR and SP VFP register */
TH_TABLE(TH_VMOV_GP_SP, "vmov.gp_sp", {&SHAPE_VMOVGPSP, 0xee000a10, vmov_gp_sp_emit});

/* VMOV between two GPRs and DP VFP register */
TH_TABLE(TH_VMOV_2GP_DP, "vmov.2gp_dp", {&SHAPE_VMOV2GPDP, 0xec400b10, vmov_2gp_dp_emit});

/* VMRS */
TH_TABLE(TH_VMRS, "vmrs", {&SHAPE_VMRS, 0xeef10a10, NULL});

/* VCVT.F64.F32 (SP -> DP) */
TH_TABLE(TH_VCVT_FD, "vcvt.f64.f32", {&SHAPE_VCVT_FD, 0xeeb70ac0, vcvt_fd_emit});

/* VCVT.F32.F64 (DP -> SP) */
TH_TABLE(TH_VCVT_DF, "vcvt.f32.f64", {&SHAPE_VCVT_DF, 0xeeb70bc0, vcvt_df_emit});

/* VCVT fp/int SP / DP */
TH_TABLE(TH_VCVT_FP_INT_SP, "vcvt.fp_int.f32", {&SHAPE_VCVT_FP_INT_SP, 0xeeb80a40, vcvt_fp_int_emit});
TH_TABLE(TH_VCVT_FP_INT_DP, "vcvt.fp_int.f64", {&SHAPE_VCVT_FP_INT_DP, 0xeeb80b40, vcvt_fp_int_emit});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_vadd_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit(TH_VADD_F_SP.name, TH_VADD_F_SP.variants, TH_VADD_F_SP.variant_count,
                     (thop_args){.rd = vd, .rn = vn, .rm = vm});
  return thop_emit(TH_VADD_F_DP.name, TH_VADD_F_DP.variants, TH_VADD_F_DP.variant_count,
                   (thop_args){.rd = vd, .rn = vn, .rm = vm});
}

thumb_opcode th_vsub_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit(TH_VSUB_F_SP.name, TH_VSUB_F_SP.variants, TH_VSUB_F_SP.variant_count,
                     (thop_args){.rd = vd, .rn = vn, .rm = vm});
  return thop_emit(TH_VSUB_F_DP.name, TH_VSUB_F_DP.variants, TH_VSUB_F_DP.variant_count,
                   (thop_args){.rd = vd, .rn = vn, .rm = vm});
}

thumb_opcode th_vmul_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit(TH_VMUL_F_SP.name, TH_VMUL_F_SP.variants, TH_VMUL_F_SP.variant_count,
                     (thop_args){.rd = vd, .rn = vn, .rm = vm});
  return thop_emit(TH_VMUL_F_DP.name, TH_VMUL_F_DP.variants, TH_VMUL_F_DP.variant_count,
                   (thop_args){.rd = vd, .rn = vn, .rm = vm});
}

thumb_opcode th_vdiv_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit(TH_VDIV_F_SP.name, TH_VDIV_F_SP.variants, TH_VDIV_F_SP.variant_count,
                     (thop_args){.rd = vd, .rn = vn, .rm = vm});
  return thop_emit(TH_VDIV_F_DP.name, TH_VDIV_F_DP.variants, TH_VDIV_F_DP.variant_count,
                   (thop_args){.rd = vd, .rn = vn, .rm = vm});
}

thumb_opcode th_vneg_f(uint32_t vd, uint32_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit(TH_VNEG_F_SP.name, TH_VNEG_F_SP.variants, TH_VNEG_F_SP.variant_count,
                     (thop_args){.rd = vd, .rm = vm});
  return thop_emit(TH_VNEG_F_DP.name, TH_VNEG_F_DP.variants, TH_VNEG_F_DP.variant_count,
                   (thop_args){.rd = vd, .rm = vm});
}

thumb_opcode th_vcmp_f(uint32_t vd, uint32_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit(TH_VCMP_F_SP.name, TH_VCMP_F_SP.variants, TH_VCMP_F_SP.variant_count,
                     (thop_args){.rd = vd, .rm = vm});
  return thop_emit(TH_VCMP_F_DP.name, TH_VCMP_F_DP.variants, TH_VCMP_F_DP.variant_count,
                   (thop_args){.rd = vd, .rm = vm});
}

thumb_opcode th_vpush(uint32_t regs, uint32_t is_doubleword)
{
  if (is_doubleword == 0)
    return thop_emit(TH_VPUSH_SP.name, TH_VPUSH_SP.variants, TH_VPUSH_SP.variant_count, (thop_args){.imm = regs});
  return thop_emit(TH_VPUSH_DP.name, TH_VPUSH_DP.variants, TH_VPUSH_DP.variant_count, (thop_args){.imm = regs});
}

thumb_opcode th_vpop(uint32_t regs, uint32_t is_doubleword)
{
  if (is_doubleword == 0)
    return thop_emit(TH_VPOP_SP.name, TH_VPOP_SP.variants, TH_VPOP_SP.variant_count, (thop_args){.imm = regs});
  return thop_emit(TH_VPOP_DP.name, TH_VPOP_DP.variants, TH_VPOP_DP.variant_count, (thop_args){.imm = regs});
}

thumb_opcode th_vldr(uint32_t vd, uint32_t rn, int32_t offset, uint32_t is_double)
{
  uint32_t u = (offset >= 0) ? 1u : 0u;
  uint32_t imm8 = ((uint32_t)(offset < 0 ? -offset : offset) >> 2) & 0xff;
  if (is_double == 0)
    return thop_emit(TH_VLDR_SP.name, TH_VLDR_SP.variants, TH_VLDR_SP.variant_count,
                     (thop_args){.rd = vd, .rn = rn, .imm = imm8, .imm2 = u});
  return thop_emit(TH_VLDR_DP.name, TH_VLDR_DP.variants, TH_VLDR_DP.variant_count,
                   (thop_args){.rd = vd, .rn = rn, .imm = imm8, .imm2 = u});
}

thumb_opcode th_vstr(uint32_t vd, uint32_t rn, int32_t offset, uint32_t is_double)
{
  uint32_t u = (offset >= 0) ? 1u : 0u;
  uint32_t imm8 = ((uint32_t)(offset < 0 ? -offset : offset) >> 2) & 0xff;
  if (is_double == 0)
    return thop_emit(TH_VSTR_SP.name, TH_VSTR_SP.variants, TH_VSTR_SP.variant_count,
                     (thop_args){.rd = vd, .rn = rn, .imm = imm8, .imm2 = u});
  return thop_emit(TH_VSTR_DP.name, TH_VSTR_DP.variants, TH_VSTR_DP.variant_count,
                   (thop_args){.rd = vd, .rn = rn, .imm = imm8, .imm2 = u});
}

thumb_opcode th_vmov_register(uint16_t vd, uint16_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit(TH_VMOV_REG_SP.name, TH_VMOV_REG_SP.variants, TH_VMOV_REG_SP.variant_count,
                     (thop_args){.rd = vd, .rm = vm});
  return thop_emit(TH_VMOV_REG_DP.name, TH_VMOV_REG_DP.variants, TH_VMOV_REG_DP.variant_count,
                   (thop_args){.rd = vd, .rm = vm});
}

thumb_opcode th_vmov_gp_sp(uint16_t rt, uint16_t sn, uint16_t to_arm_register)
{
  return thop_emit(TH_VMOV_GP_SP.name, TH_VMOV_GP_SP.variants, TH_VMOV_GP_SP.variant_count,
                   (thop_args){.rd = rt, .rn = sn, .imm2 = to_arm_register});
}

thumb_opcode th_vmov_2gp_dp(uint16_t rt, uint16_t rt2, uint16_t dm, uint16_t to_arm_register)
{
  return thop_emit(TH_VMOV_2GP_DP.name, TH_VMOV_2GP_DP.variants, TH_VMOV_2GP_DP.variant_count,
                   (thop_args){.rd = rt, .rn = rt2, .rm = dm, .imm2 = to_arm_register});
}

thumb_opcode th_vmrs(uint16_t rt)
{
  return thop_emit(TH_VMRS.name, TH_VMRS.variants, TH_VMRS.variant_count, (thop_args){.rd = rt});
}

thumb_opcode th_vcvt_float_to_double(uint32_t vd, uint32_t vm)
{
  return thop_emit(TH_VCVT_FD.name, TH_VCVT_FD.variants, TH_VCVT_FD.variant_count,
                   (thop_args){.rd = vd, .rm = vm});
}

thumb_opcode th_vcvt_double_to_float(uint32_t vd, uint32_t vm)
{
  return thop_emit(TH_VCVT_DF.name, TH_VCVT_DF.variants, TH_VCVT_DF.variant_count,
                   (thop_args){.rd = vd, .rm = vm});
}

thumb_opcode th_vcvt_fp_int(uint32_t vd, uint32_t vm, uint32_t opc, uint32_t is_double, uint32_t op)
{
  if (is_double == 0)
    return thop_emit(TH_VCVT_FP_INT_SP.name, TH_VCVT_FP_INT_SP.variants, TH_VCVT_FP_INT_SP.variant_count,
                     (thop_args){.rd = vd, .rm = vm, .imm = opc, .imm2 = op, .puw = 0});
  return thop_emit(TH_VCVT_FP_INT_DP.name, TH_VCVT_FP_INT_DP.variants, TH_VCVT_FP_INT_DP.variant_count,
                   (thop_args){.rd = vd, .rm = vm, .imm = opc, .imm2 = op, .puw = 1});
}

thumb_opcode th_vcvt_convert(uint32_t vd, uint32_t vm, const char *dest_type, const char *src_type)
{
  if ((strcmp(dest_type, "s32") == 0 || strcmp(dest_type, "u32") == 0) && strcmp(src_type, "f32") == 0)
  {
    int is_unsigned = strcmp(dest_type, "u32") == 0;
    return th_vcvt_fp_int(vd, vm, is_unsigned ? 0x4 : 0x5, 0, 1);
  }
  else if ((strcmp(dest_type, "s32") == 0 || strcmp(dest_type, "u32") == 0) && strcmp(src_type, "f64") == 0)
  {
    int is_unsigned = strcmp(dest_type, "u32") == 0;
    return th_vcvt_fp_int(vd, vm, is_unsigned ? 0x4 : 0x5, 1, 1);
  }
  else if ((strcmp(dest_type, "f32") == 0 || strcmp(dest_type, "f64") == 0) &&
           (strcmp(src_type, "s32") == 0 || strcmp(src_type, "u32") == 0))
  {
    int dst_is_double = strcmp(dest_type, "f64") == 0;
    int is_unsigned = strcmp(src_type, "u32") == 0;
    return th_vcvt_fp_int(vd, vm, 0, dst_is_double, is_unsigned ? 0 : 1);
  }
  else if (strcmp(dest_type, "f64") == 0 && strcmp(src_type, "f32") == 0)
  {
    return th_vcvt_float_to_double(vd / 2, vm);
  }
  else if (strcmp(dest_type, "f32") == 0 && strcmp(src_type, "f64") == 0)
  {
    return th_vcvt_double_to_float(vd, vm / 2);
  }
  return (thumb_opcode){.size = 0, .opcode = 0};
}
