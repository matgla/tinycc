/* thop_customs_def.h -- the custom emitters by id (thop_shapes.h).
 * Included once, by thumb.c: direct calls, so no function-pointer table
 * (and no per-process RAM thunks on YasOS). */

thumb_opcode bl_t1_emit(uint32_t base, const thop_args *a);
thumb_opcode b_t3_emit(uint32_t base, const thop_args *a);
thumb_opcode b_t4_emit(uint32_t base, const thop_args *a);
thumb_opcode b_t2_emit(uint32_t base, const thop_args *a);
thumb_opcode cbz_emit(uint32_t base, const thop_args *a);
thumb_opcode cmp_reg_t2_custom_emit(uint32_t base, const thop_args *a);
thumb_opcode coproc_cdp_emit(uint32_t base, const thop_args *a);
thumb_opcode coproc_mcr_emit(uint32_t base, const thop_args *a);
thumb_opcode coproc_mcrr_emit(uint32_t base, const thop_args *a);
thumb_opcode pkhbt_emit(uint32_t base, const thop_args *a);
thumb_opcode ldr_literal_emit(uint32_t base, const thop_args *a);
thumb_opcode ldrd_imm_emit(uint32_t base, const thop_args *a);
thumb_opcode mov_reg_t1_shift_emit(uint32_t base, const thop_args *a);
thumb_opcode mov_imm_t1_emit(uint32_t base, const thop_args *a);
thumb_opcode mov_reg_shift_t1_emit(uint32_t base, const thop_args *a);
thumb_opcode mrs_emit(uint32_t base, const thop_args *a);
thumb_opcode msr_emit(uint32_t base, const thop_args *a);
thumb_opcode mul_t16_emit(uint32_t base, const thop_args *a);
thumb_opcode long_mul_emit(uint32_t base, const thop_args *a);
thumb_opcode mvn_imm_emit(uint32_t base, const thop_args *a);
thumb_opcode shift_reg_t1_emit(uint32_t base, const thop_args *a);
thumb_opcode clz_emit(uint32_t base, const thop_args *a);
thumb_opcode tt_emit(uint32_t base, const thop_args *a);
thumb_opcode vfp_arith3_emit(uint32_t base, const thop_args *a);
thumb_opcode vfp_arith2_emit(uint32_t base, const thop_args *a);
thumb_opcode vmov_reg_emit(uint32_t base, const thop_args *a);
thumb_opcode vfp_pushpop_emit(uint32_t base, const thop_args *a);
thumb_opcode vfp_ldst_emit(uint32_t base, const thop_args *a);
thumb_opcode vmov_gp_sp_emit(uint32_t base, const thop_args *a);
thumb_opcode vmov_2gp_dp_emit(uint32_t base, const thop_args *a);
thumb_opcode vcvt_fd_emit(uint32_t base, const thop_args *a);
thumb_opcode vcvt_df_emit(uint32_t base, const thop_args *a);
thumb_opcode vcvt_fp_int_emit(uint32_t base, const thop_args *a);

static thumb_opcode thop_call_custom(int id, uint32_t base, const thop_args *a)
{
  switch (id)
  {
  case THOP_CUSTOM_bl_t1_emit:
    return bl_t1_emit(base, a);
  case THOP_CUSTOM_b_t3_emit:
    return b_t3_emit(base, a);
  case THOP_CUSTOM_b_t4_emit:
    return b_t4_emit(base, a);
  case THOP_CUSTOM_b_t2_emit:
    return b_t2_emit(base, a);
  case THOP_CUSTOM_cbz_emit:
    return cbz_emit(base, a);
  case THOP_CUSTOM_cmp_reg_t2_custom_emit:
    return cmp_reg_t2_custom_emit(base, a);
  case THOP_CUSTOM_coproc_cdp_emit:
    return coproc_cdp_emit(base, a);
  case THOP_CUSTOM_coproc_mcr_emit:
    return coproc_mcr_emit(base, a);
  case THOP_CUSTOM_coproc_mcrr_emit:
    return coproc_mcrr_emit(base, a);
  case THOP_CUSTOM_pkhbt_emit:
    return pkhbt_emit(base, a);
  case THOP_CUSTOM_ldr_literal_emit:
    return ldr_literal_emit(base, a);
  case THOP_CUSTOM_ldrd_imm_emit:
    return ldrd_imm_emit(base, a);
  case THOP_CUSTOM_mov_reg_t1_shift_emit:
    return mov_reg_t1_shift_emit(base, a);
  case THOP_CUSTOM_mov_imm_t1_emit:
    return mov_imm_t1_emit(base, a);
  case THOP_CUSTOM_mov_reg_shift_t1_emit:
    return mov_reg_shift_t1_emit(base, a);
  case THOP_CUSTOM_mrs_emit:
    return mrs_emit(base, a);
  case THOP_CUSTOM_msr_emit:
    return msr_emit(base, a);
  case THOP_CUSTOM_mul_t16_emit:
    return mul_t16_emit(base, a);
  case THOP_CUSTOM_long_mul_emit:
    return long_mul_emit(base, a);
  case THOP_CUSTOM_mvn_imm_emit:
    return mvn_imm_emit(base, a);
  case THOP_CUSTOM_shift_reg_t1_emit:
    return shift_reg_t1_emit(base, a);
  case THOP_CUSTOM_clz_emit:
    return clz_emit(base, a);
  case THOP_CUSTOM_tt_emit:
    return tt_emit(base, a);
  case THOP_CUSTOM_vfp_arith3_emit:
    return vfp_arith3_emit(base, a);
  case THOP_CUSTOM_vfp_arith2_emit:
    return vfp_arith2_emit(base, a);
  case THOP_CUSTOM_vmov_reg_emit:
    return vmov_reg_emit(base, a);
  case THOP_CUSTOM_vfp_pushpop_emit:
    return vfp_pushpop_emit(base, a);
  case THOP_CUSTOM_vfp_ldst_emit:
    return vfp_ldst_emit(base, a);
  case THOP_CUSTOM_vmov_gp_sp_emit:
    return vmov_gp_sp_emit(base, a);
  case THOP_CUSTOM_vmov_2gp_dp_emit:
    return vmov_2gp_dp_emit(base, a);
  case THOP_CUSTOM_vcvt_fd_emit:
    return vcvt_fd_emit(base, a);
  case THOP_CUSTOM_vcvt_df_emit:
    return vcvt_df_emit(base, a);
  case THOP_CUSTOM_vcvt_fp_int_emit:
    return vcvt_fp_int_emit(base, a);
  default:
    return (thumb_opcode){.size = 0, .opcode = 0};
  }
}
