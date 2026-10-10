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
thumb_opcode vfp_arith3_emit(uint32_t base, const thop_args *a)
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
thumb_opcode vfp_arith2_emit(uint32_t base, const thop_args *a)
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
thumb_opcode vmov_reg_emit(uint32_t base, const thop_args *a)
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
thumb_opcode vfp_pushpop_emit(uint32_t base, const thop_args *a)
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
thumb_opcode vfp_ldst_emit(uint32_t base, const thop_args *a)
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
thumb_opcode vmov_gp_sp_emit(uint32_t base, const thop_args *a)
{
  uint32_t Vn = (a->rn >> 1) & 0xf;
  uint32_t N = a->rn & 1;
  uint32_t op = base | (a->imm2 << 20) | (a->rd << 12) | (Vn << 16) | (N << 7);
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* VMOV between two GPRs and double-precision VFP register */
thumb_opcode vmov_2gp_dp_emit(uint32_t base, const thop_args *a)
{
  uint32_t M = (a->rm >> 4) & 1;
  uint32_t Vm = a->rm & 0xf;
  uint32_t op = base | (a->imm2 << 20) | (a->rn << 16) | (a->rd << 12) | (M << 5) | Vm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* VCVT float-to-double and double-to-float */
thumb_opcode vcvt_fd_emit(uint32_t base, const thop_args *a)
{
  uint32_t D = (a->rd >> 4) & 1;
  uint32_t Vd = a->rd & 0xf;
  uint32_t M = a->rm & 1;
  uint32_t Vm = (a->rm >> 1) & 0xf;
  uint32_t op = base | (D << 22) | (Vd << 12) | (M << 5) | Vm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

thumb_opcode vcvt_df_emit(uint32_t base, const thop_args *a)
{
  uint32_t D = a->rd & 1;
  uint32_t Vd = (a->rd >> 1) & 0xf;
  uint32_t M = (a->rm >> 4) & 1;
  uint32_t Vm = a->rm & 0xf;
  uint32_t op = base | (D << 22) | (Vd << 12) | (M << 5) | Vm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

/* VCVT between floating-point and integer */
thumb_opcode vcvt_fp_int_emit(uint32_t base, const thop_args *a)
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

/* ═══════════════════════════════════════════════════════════════════
 *  THOP tables
 * ═══════════════════════════════════════════════════════════════════ */

/* VADD.F32 / VADD.F64 */
TH_TABLE(TH_VADD_F_SP, "vadd.f32", {THOP_SHAPE_VFP_SP, 0xee300a00, THOP_CUSTOM_vfp_arith3_emit});
TH_TABLE(TH_VADD_F_DP, "vadd.f64", {THOP_SHAPE_VFP_DP, 0xee300b00, THOP_CUSTOM_vfp_arith3_emit});

/* VSUB.F32 / VSUB.F64 */
TH_TABLE(TH_VSUB_F_SP, "vsub.f32", {THOP_SHAPE_VFP_SP, 0xee300a40, THOP_CUSTOM_vfp_arith3_emit});
TH_TABLE(TH_VSUB_F_DP, "vsub.f64", {THOP_SHAPE_VFP_DP, 0xee300b40, THOP_CUSTOM_vfp_arith3_emit});

/* VMUL.F32 / VMUL.F64 */
TH_TABLE(TH_VMUL_F_SP, "vmul.f32", {THOP_SHAPE_VFP_SP, 0xee200a00, THOP_CUSTOM_vfp_arith3_emit});
TH_TABLE(TH_VMUL_F_DP, "vmul.f64", {THOP_SHAPE_VFP_DP, 0xee200b00, THOP_CUSTOM_vfp_arith3_emit});

/* VDIV.F32 / VDIV.F64 */
TH_TABLE(TH_VDIV_F_SP, "vdiv.f32", {THOP_SHAPE_VFP_SP, 0xee800a00, THOP_CUSTOM_vfp_arith3_emit});
TH_TABLE(TH_VDIV_F_DP, "vdiv.f64", {THOP_SHAPE_VFP_DP, 0xee800b00, THOP_CUSTOM_vfp_arith3_emit});

/* VNEG.F32 / VNEG.F64 */
TH_TABLE(TH_VNEG_F_SP, "vneg.f32", {THOP_SHAPE_VFP_SP, 0xeeb10a40, THOP_CUSTOM_vfp_arith2_emit});
TH_TABLE(TH_VNEG_F_DP, "vneg.f64", {THOP_SHAPE_VFP_DP, 0xeeb10b40, THOP_CUSTOM_vfp_arith2_emit});

/* VCMP.F32 / VCMP.F64 */
TH_TABLE(TH_VCMP_F_SP, "vcmp.f32", {THOP_SHAPE_VFP_SP, 0xeeb40a40, THOP_CUSTOM_vfp_arith2_emit});
TH_TABLE(TH_VCMP_F_DP, "vcmp.f64", {THOP_SHAPE_VFP_DP, 0xeeb40b40, THOP_CUSTOM_vfp_arith2_emit});

/* VPUSH SP / DP */
TH_TABLE(TH_VPUSH_SP, "vpush.f32", {THOP_SHAPE_VFP_SP, 0xed2d0a00, THOP_CUSTOM_vfp_pushpop_emit});
TH_TABLE(TH_VPUSH_DP, "vpush.f64", {THOP_SHAPE_VFP_SP, 0xed2d0b00, THOP_CUSTOM_vfp_pushpop_emit});

/* VPOP SP / DP */
TH_TABLE(TH_VPOP_SP, "vpop.f32", {THOP_SHAPE_VFP_SP, 0xecbd0a00, THOP_CUSTOM_vfp_pushpop_emit});
TH_TABLE(TH_VPOP_DP, "vpop.f64", {THOP_SHAPE_VFP_SP, 0xecbd0b00, THOP_CUSTOM_vfp_pushpop_emit});

/* VLDR SP / DP */
TH_TABLE(TH_VLDR_SP, "vldr.f32", {THOP_SHAPE_VFP_SP, 0xed900a00, THOP_CUSTOM_vfp_ldst_emit});
TH_TABLE(TH_VLDR_DP, "vldr.f64", {THOP_SHAPE_VFP_DP_MOVE, 0xed900b00, THOP_CUSTOM_vfp_ldst_emit});

/* VSTR SP / DP */
TH_TABLE(TH_VSTR_SP, "vstr.f32", {THOP_SHAPE_VFP_SP, 0xed800a00, THOP_CUSTOM_vfp_ldst_emit});
TH_TABLE(TH_VSTR_DP, "vstr.f64", {THOP_SHAPE_VFP_DP_MOVE, 0xed800b00, THOP_CUSTOM_vfp_ldst_emit});

/* VMOV register SP / DP */
TH_TABLE(TH_VMOV_REG_SP, "vmov.f32", {THOP_SHAPE_VFP_SP, 0xeeb00a40, THOP_CUSTOM_vmov_reg_emit});
TH_TABLE(TH_VMOV_REG_DP, "vmov.f64", {THOP_SHAPE_VFP_DP, 0xeeb00b40, THOP_CUSTOM_vmov_reg_emit});

/* VMOV between GPR and SP VFP register */
TH_TABLE(TH_VMOV_GP_SP, "vmov.gp_sp", {THOP_SHAPE_VMOVGPSP, 0xee000a10, THOP_CUSTOM_vmov_gp_sp_emit});

/* VMOV between two GPRs and DP VFP register */
TH_TABLE(TH_VMOV_2GP_DP, "vmov.2gp_dp", {THOP_SHAPE_VMOV2GPDP, 0xec400b10, THOP_CUSTOM_vmov_2gp_dp_emit});

/* VMRS */
TH_TABLE(TH_VMRS, "vmrs", {THOP_SHAPE_VMRS, 0xeef10a10, 0});

/* VCVT.F64.F32 (SP -> DP) */
TH_TABLE(TH_VCVT_FD, "vcvt.f64.f32", {THOP_SHAPE_VCVT_FD, 0xeeb70ac0, THOP_CUSTOM_vcvt_fd_emit});

/* VCVT.F32.F64 (DP -> SP) */
TH_TABLE(TH_VCVT_DF, "vcvt.f32.f64", {THOP_SHAPE_VCVT_DF, 0xeeb70bc0, THOP_CUSTOM_vcvt_df_emit});

/* VCVT fp/int SP / DP */
TH_TABLE(TH_VCVT_FP_INT_SP, "vcvt.fp_int.f32", {THOP_SHAPE_VCVT_FP_INT_SP, 0xeeb80a40, THOP_CUSTOM_vcvt_fp_int_emit});
TH_TABLE(TH_VCVT_FP_INT_DP, "vcvt.fp_int.f64", {THOP_SHAPE_VCVT_FP_INT_DP, 0xeeb80b40, THOP_CUSTOM_vcvt_fp_int_emit});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_vadd_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit_table(&TH_VADD_F_SP, (thop_args){.rd = vd, .rn = vn, .rm = vm});
  return thop_emit_table(&TH_VADD_F_DP, (thop_args){.rd = vd, .rn = vn, .rm = vm});
}

thumb_opcode th_vsub_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit_table(&TH_VSUB_F_SP, (thop_args){.rd = vd, .rn = vn, .rm = vm});
  return thop_emit_table(&TH_VSUB_F_DP, (thop_args){.rd = vd, .rn = vn, .rm = vm});
}

thumb_opcode th_vmul_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit_table(&TH_VMUL_F_SP, (thop_args){.rd = vd, .rn = vn, .rm = vm});
  return thop_emit_table(&TH_VMUL_F_DP, (thop_args){.rd = vd, .rn = vn, .rm = vm});
}

thumb_opcode th_vdiv_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit_table(&TH_VDIV_F_SP, (thop_args){.rd = vd, .rn = vn, .rm = vm});
  return thop_emit_table(&TH_VDIV_F_DP, (thop_args){.rd = vd, .rn = vn, .rm = vm});
}

thumb_opcode th_vneg_f(uint32_t vd, uint32_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit_table(&TH_VNEG_F_SP, (thop_args){.rd = vd, .rm = vm});
  return thop_emit_table(&TH_VNEG_F_DP, (thop_args){.rd = vd, .rm = vm});
}

thumb_opcode th_vcmp_f(uint32_t vd, uint32_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit_table(&TH_VCMP_F_SP, (thop_args){.rd = vd, .rm = vm});
  return thop_emit_table(&TH_VCMP_F_DP, (thop_args){.rd = vd, .rm = vm});
}

thumb_opcode th_vpush(uint32_t regs, uint32_t is_doubleword)
{
  if (is_doubleword == 0)
    return thop_emit_table(&TH_VPUSH_SP, (thop_args){.imm = regs});
  return thop_emit_table(&TH_VPUSH_DP, (thop_args){.imm = regs});
}

thumb_opcode th_vpop(uint32_t regs, uint32_t is_doubleword)
{
  if (is_doubleword == 0)
    return thop_emit_table(&TH_VPOP_SP, (thop_args){.imm = regs});
  return thop_emit_table(&TH_VPOP_DP, (thop_args){.imm = regs});
}

/* VLDM / VSTM, increment-after or decrement-before, any base register.
 * vpush is VSTMDB SP! and vpop VLDMIA SP!: the same encoding with Rn = 13,
 * so the register list packs the same way.  DB requires writeback. */
thumb_opcode th_vldmstm(int load, int decrement_before, uint32_t rn, int writeback, uint32_t regs,
                        uint32_t is_doubleword)
{
  uint32_t base = 0xec000a00u | (is_doubleword ? 0x100u : 0u) | ((uint32_t)!!load << 20) |
                  ((uint32_t)!!writeback << 21) | ((rn & 0xfu) << 16) |
                  (decrement_before ? (1u << 24) : (1u << 23));
  return vfp_pushpop_emit(base, &(thop_args){.imm = regs});
}

thumb_opcode th_vldr(uint32_t vd, uint32_t rn, int32_t offset, uint32_t is_double)
{
  uint32_t u = (offset >= 0) ? 1u : 0u;
  uint32_t imm8 = ((uint32_t)(offset < 0 ? -offset : offset) >> 2) & 0xff;
  if (is_double == 0)
    return thop_emit_table(&TH_VLDR_SP, (thop_args){.rd = vd, .rn = rn, .imm = imm8, .imm2 = u});
  return thop_emit_table(&TH_VLDR_DP, (thop_args){.rd = vd, .rn = rn, .imm = imm8, .imm2 = u});
}

thumb_opcode th_vstr(uint32_t vd, uint32_t rn, int32_t offset, uint32_t is_double)
{
  uint32_t u = (offset >= 0) ? 1u : 0u;
  uint32_t imm8 = ((uint32_t)(offset < 0 ? -offset : offset) >> 2) & 0xff;
  if (is_double == 0)
    return thop_emit_table(&TH_VSTR_SP, (thop_args){.rd = vd, .rn = rn, .imm = imm8, .imm2 = u});
  return thop_emit_table(&TH_VSTR_DP, (thop_args){.rd = vd, .rn = rn, .imm = imm8, .imm2 = u});
}

thumb_opcode th_vmov_register(uint16_t vd, uint16_t vm, uint32_t sz)
{
  if (sz == 0)
    return thop_emit_table(&TH_VMOV_REG_SP, (thop_args){.rd = vd, .rm = vm});
  return thop_emit_table(&TH_VMOV_REG_DP, (thop_args){.rd = vd, .rm = vm});
}

thumb_opcode th_vmov_gp_sp(uint16_t rt, uint16_t sn, uint16_t to_arm_register)
{
  return thop_emit_table(&TH_VMOV_GP_SP, (thop_args){.rd = rt, .rn = sn, .imm2 = to_arm_register});
}

thumb_opcode th_vmov_2gp_dp(uint16_t rt, uint16_t rt2, uint16_t dm, uint16_t to_arm_register)
{
  return thop_emit_table(&TH_VMOV_2GP_DP, (thop_args){.rd = rt, .rn = rt2, .rm = dm, .imm2 = to_arm_register});
}

thumb_opcode th_vmrs(uint16_t rt)
{
  return thop_emit_table(&TH_VMRS, (thop_args){.rd = rt});
}

thumb_opcode th_vcvt_float_to_double(uint32_t vd, uint32_t vm)
{
  return thop_emit_table(&TH_VCVT_FD, (thop_args){.rd = vd, .rm = vm});
}

thumb_opcode th_vcvt_double_to_float(uint32_t vd, uint32_t vm)
{
  return thop_emit_table(&TH_VCVT_DF, (thop_args){.rd = vd, .rm = vm});
}

thumb_opcode th_vcvt_fp_int(uint32_t vd, uint32_t vm, uint32_t opc, uint32_t is_double, uint32_t op)
{
  if (is_double == 0)
    return thop_emit_table(&TH_VCVT_FP_INT_SP, (thop_args){.rd = vd, .rm = vm, .imm = opc, .imm2 = op, .puw = 0});
  return thop_emit_table(&TH_VCVT_FP_INT_DP, (thop_args){.rd = vd, .rm = vm, .imm = opc, .imm2 = op, .puw = 1});
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
