/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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
#include "source/backend/arch/arm/arm.h"
#include "source/backend/arch/arm/thumb/thumb.h"
#include "tcc.h"

/* ───── Internal profile / FPU resolution ───── */

static const FloatingPointConfig *arm_resolve_fpu(const char *mfpu)
{
  /* TODO: link to actual FPU configs once they live under arch/arm/fpu/ */
  (void)mfpu;
  return NULL;
}

struct target_dependent_config arm_target_dependent;

ArchitectureConfig architecture_config;

void arm_target_init(const char *march, const char *mfpu, const char *mcpu, uint64_t extra_feat_bits)
{
  thop_feat feat = thumb_resolve_features(march, mfpu, extra_feat_bits);

  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = mcpu,
      .feat = feat,
      .is_secure_tz = feat.sec != 0,
  };

  architecture_config = (ArchitectureConfig){
      .pointer_size = 4,
      .stack_align = 8,
      .reg_size = 4,
      .parameter_registers = 4,
      .has_fpu = 0,
      .static_chain_reg = 10,
      .fpu = NULL,

      .march_name = march ? march : "armv8-m.main",
      .int_reg_count = 13,
      .fp_reg_count = feat.fp_dp_d32 ? 64
                      : feat.vfp_dp  ? 32
                      : feat.vfp_sp  ? 32
                                     : 0,
      .default_align = 4,
      .big_endian = 0,

      .target_dependent = &arm_target_dependent,
  };

  if (mfpu)
  {
    const FloatingPointConfig *fpu = arm_resolve_fpu(mfpu);
    architecture_config.fpu = fpu;
    architecture_config.has_fpu = fpu != NULL;
  }
}

bool tcc_target_has(tcc_target_cap cap)
{
  const thop_feat f = arm_target_dependent.feat;
  switch (cap)
  {
  case TCC_CAP_HW_DIVIDE:
    return f.div;
  case TCC_CAP_HW_FP_SP:
    return f.vfp_sp;
  case TCC_CAP_HW_FP_DP:
    return f.vfp_dp;
  case TCC_CAP_HW_FP_HP:
    return f.fp16;
  case TCC_CAP_DSP_SIMD:
    return f.dsp;
  case TCC_CAP_SATURATING_ARITH:
    return f.sat;
  case TCC_CAP_BITFIELD_INSTRS:
    return f.bfx;
  case TCC_CAP_COND_EXEC:
    return f.it;
  case TCC_CAP_MOVE_IMM_WIDE:
    return f.movw_movt;
  case TCC_CAP_VECTOR:
    return f.mve_int;
  case TCC_CAP_SECURITY:
    return f.sec;
  case TCC_CAP_POINTER_AUTH:
    return f.pacbti;
  case TCC_CAP_LOW_OVERHEAD_LOOP:
    return f.lob;
  }
  return false;
}
