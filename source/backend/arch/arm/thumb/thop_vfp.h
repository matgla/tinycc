#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_vadd_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz);
thumb_opcode th_vsub_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz);
thumb_opcode th_vmul_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz);
thumb_opcode th_vdiv_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz);
thumb_opcode th_vneg_f(uint32_t vd, uint32_t vm, uint32_t sz);
thumb_opcode th_vcmp_f(uint32_t vd, uint32_t vm, uint32_t sz);
thumb_opcode th_vpush(uint32_t regs, uint32_t is_doubleword);
thumb_opcode th_vpop(uint32_t regs, uint32_t is_doubleword);
thumb_opcode th_vldr(uint32_t vd, uint32_t rn, int32_t offset, uint32_t is_double);
thumb_opcode th_vstr(uint32_t vd, uint32_t rn, int32_t offset, uint32_t is_double);
thumb_opcode th_vmov_register(uint16_t vd, uint16_t vm, uint32_t sz);
thumb_opcode th_vmov_gp_sp(uint16_t rt, uint16_t sn, uint16_t to_arm_register);
thumb_opcode th_vmov_2gp_dp(uint16_t rt, uint16_t rt2, uint16_t dm, uint16_t to_arm_register);
thumb_opcode th_vmrs(uint16_t rt);
thumb_opcode th_vcvt_float_to_double(uint32_t vd, uint32_t vm);
thumb_opcode th_vcvt_double_to_float(uint32_t vd, uint32_t vm);
thumb_opcode th_vcvt_fp_int(uint32_t vd, uint32_t vm, uint32_t opc, uint32_t is_double, uint32_t op);
thumb_opcode th_vcvt_convert(uint32_t vd, uint32_t vm, const char *dest_type, const char *src_type);
