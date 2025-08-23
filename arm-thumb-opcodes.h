/*
 *  ARMvX-m opcodes for TCC
 *  Uses thumb instruction set
 *
 *  Based on:
 *  ARM Thumb 2 instruction functions for TCC
 *  Copyright (c) 2020 Erlend J. Sveen
 *  from:
 * https://git.erlendjs.no/erlendjs/tinycc/-/blob/arm-thumb/arm-thumb-gen.c
 *       https://git.erlendjs.no/erlendjs/tinycc/-/blob/arm-thumb/arm-thumb-instructions.c
 *
 *  And
 *
 *  ARMv4 code generator for TCC
 *
 *  Copyright (c) 2003 Daniel Glöckner
 *  Copyright (c) 2012 Thomas Preud'homme
 *
 *  Based on i386-gen.c by Fabrice Bellard
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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifndef TCC_DEBUG
#define TCC_DEBUG 0
#endif

#define TRACE(...)
#define LOG(...)

#if TCC_DEBUG == 1 || TCC_DEBUG == 2
#undef LOG
#define LOG(...)                                                               \
  printf("[INF]: ");                                                           \
  printf(__VA_ARGS__);                                                         \
  printf("\n")
#endif

#if TCC_DEBUG == 2
#undef TRACE
#define TRACE(...)                                                             \
  printf("[TRC]: ");                                                           \
  printf(__VA_ARGS__);                                                         \
  printf("\n")
#endif

#define ceil_div(x, d) ((x + (d - 1)) / d)

#define R0 0
#define R1 1
#define R2 2
#define R3 3
#define R4 4
#define R5 5
#define R6 6
#define R7 7
#define R8 8
#define R9 9
#define R10 10
#define R_FP 11
#define R_IP 12
#define R_SP 13
#define R_LR 14
#define R_PC 15

typedef enum {
  FLAGS_BEHAVIOUR_NOT_IMPORTANT = 0,
  FLAGS_BEHAVIOUR_SET = 1,
  FLAGS_BEHAVIOUR_BLOCK = 2,
} flags_behaviour;

typedef enum {
  ENFORCE_ENCODING_NONE = 0,
  ENFORCE_ENCODING_16BIT = 1,
  ENFORCE_ENCODING_32BIT = 2,
} enforce_encoding;

typedef struct thumb_opcode {
  uint8_t size;
  uint32_t opcode;
} thumb_opcode;

typedef enum thumb_shift_type {
  THUMB_SHIFT_NONE,
  THUMB_SHIFT_RRX,
  THUMB_SHIFT_LSL,
  THUMB_SHIFT_LSR,
  THUMB_SHIFT_ASR,
  THUMB_SHIFT_ROR,
} thumb_shift_type;

typedef struct thumb_shift {
  thumb_shift_type type;
  uint32_t value;
} thumb_shift;

uint32_t th_packimm_10_11_0(uint32_t imm);
uint32_t th_pack_const(uint32_t imm);
uint32_t th_encbranch_b_t3(uint32_t imm);

uint32_t th_encbranch(int pos, int addr);
uint32_t th_encbranch_8(int pos, int addr);
uint32_t th_encbranch_11(int pos, int addr);
uint32_t th_encbranch_20(int pos, int addr);
uint32_t th_encbranch_24(int pos, int addr);

thumb_opcode th_nop();

thumb_opcode th_bx_reg(uint16_t rm);
thumb_opcode th_bl_t1(uint32_t imm);
thumb_opcode th_blx_reg(uint16_t rm);
thumb_opcode th_b_t1(uint32_t cond, uint32_t imm8);
thumb_opcode th_b_t2(int32_t imm11);
thumb_opcode th_b_t3(uint32_t op, uint32_t imm);
thumb_opcode th_b_t4(int32_t imm);

thumb_opcode th_mov_reg(uint16_t rd, uint16_t rm);

thumb_opcode th_mov_imm(uint16_t rd, uint32_t imm, flags_behaviour setflags,
                        enforce_encoding encoding);

thumb_opcode th_generic_op_imm_with_status(uint16_t op, uint16_t rd,
                                           uint16_t rn, uint32_t imm,
                                           flags_behaviour setflags);
thumb_opcode th_generic_op_imm(uint16_t op, uint16_t rd, uint16_t rn,
                               uint32_t imm);

thumb_opcode th_add_reg(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_add_imm(uint16_t rd, uint16_t rn, uint32_t imm);
thumb_opcode th_bic_imm(uint16_t rd, uint16_t rn, uint32_t imm);
thumb_opcode th_and_imm(uint16_t rd, uint16_t rn, uint32_t imm);
thumb_opcode th_and_reg(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_xor_reg(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_xor_imm(uint16_t rd, uint16_t rn, uint32_t imm);
thumb_opcode th_rsb_reg(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_sub_reg(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_adc_reg(uint16_t rd, uint16_t rn, uint16_t rm,
                        flags_behaviour flags, thumb_shift shift,
                        enforce_encoding encoding);
thumb_opcode th_adc_imm(uint16_t rd, uint16_t rn, uint32_t imm,
                        flags_behaviour setflags);
thumb_opcode th_sbc_imm(uint16_t rd, uint16_t rn, uint32_t imm);
thumb_opcode th_orr_imm(uint16_t rd, uint16_t rn, uint32_t imm);
thumb_opcode th_sbc_reg(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_cmp_reg(uint16_t rn, uint16_t rm);
thumb_opcode th_orr_reg(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_sub_imm(uint16_t rd, uint16_t rn, uint32_t imm);

thumb_opcode th_push(uint16_t regs);
int th_ldr_literal_estimate(uint16_t rt, uint32_t imm);
thumb_opcode th_ldrsh_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw);
thumb_opcode th_ldrsh_reg(uint32_t rt, uint32_t rn, uint32_t rm);
thumb_opcode th_ldrh_imm(uint16_t rt, uint16_t rn, uint32_t imm, uint32_t puw);
thumb_opcode th_ldrh_reg(uint32_t rt, uint32_t rn, uint32_t rm);
thumb_opcode th_ldrsb_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw);
thumb_opcode th_ldrsb_reg(uint32_t rt, uint32_t rn, uint32_t rm);
thumb_opcode th_ldrb_imm(uint16_t rt, uint16_t rn, uint32_t imm, uint32_t puw);
thumb_opcode th_ldrb_reg(uint32_t rt, uint32_t rn, uint32_t rm);
thumb_opcode th_ldr_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw);
thumb_opcode th_ldr_reg(uint32_t rt, uint32_t rn, uint32_t rm);
thumb_opcode th_ldr_literal(uint16_t rt, uint32_t imm, uint32_t add);

thumb_opcode th_pop(uint16_t regs);
thumb_opcode th_strh_imm(uint16_t rt, uint16_t rn, uint32_t imm, uint16_t puw);
thumb_opcode th_strh_reg(uint32_t rt, uint32_t rn, uint32_t rm);
thumb_opcode th_strb_imm(uint16_t rt, uint16_t rn, uint32_t imm, uint16_t puw);
thumb_opcode th_strb_reg(uint32_t rt, uint32_t rn, uint32_t rm);
thumb_opcode th_str_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw);
thumb_opcode th_str_reg(uint32_t rt, uint32_t rn, uint32_t rm);

thumb_opcode th_mul(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_umull(uint32_t rdlo, uint32_t rdhi, uint16_t rn, uint16_t rm);
thumb_opcode th_udiv(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_sdiv(uint16_t rd, uint16_t rn, uint16_t rm);

thumb_opcode th_add_sp_imm(uint16_t rd, uint32_t imm);
thumb_opcode th_add_sp_reg(uint16_t rdm);
thumb_opcode th_rsb_imm(uint16_t rd, uint16_t rn, uint32_t imm,
                        flags_behaviour setflags);
thumb_opcode th_shift_armv7m(uint16_t rd, uint16_t rm, uint32_t imm,
                             uint32_t type);
thumb_opcode th_lsl_reg(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_lsl_imm(uint16_t rd, uint16_t rm, uint32_t imm);
thumb_opcode th_lsr_reg(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_lsr_imm(uint16_t rd, uint16_t rm, uint32_t imm);
thumb_opcode th_asr_reg(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_asr_imm(uint16_t rd, uint16_t rm, uint32_t imm);

thumb_opcode th_cmp_imm(uint16_t rm, uint32_t imm, enforce_encoding encoding);

thumb_opcode th_vpush(uint32_t regs);
thumb_opcode th_vpop(uint32_t regs);
thumb_opcode th_vmov_register(uint16_t vd, uint16_t vm);
thumb_opcode th_vldr(uint32_t rn, uint32_t vd, uint32_t add,
                     uint32_t is_doubleword, uint32_t imm);
thumb_opcode th_vstr(uint32_t rn, uint32_t vd, uint32_t add,
                     uint32_t is_doubleword, uint32_t imm);
thumb_opcode th_vmov_gp_sp(uint16_t rt, uint16_t sn, uint16_t to_arm_register);
thumb_opcode th_vmov_2gp_dp(uint16_t rt, uint16_t rt2, uint16_t dm,
                            uint16_t to_arm_register);
thumb_opcode gen_th_sub_sp_imm(uint16_t rd, uint32_t imm);
uint32_t th_sub_sp_imm_estimate(uint16_t rd, uint32_t imm);
thumb_opcode th_sub_sp_imm(uint16_t rd, uint32_t imm);
thumb_opcode th_vmrs(uint16_t rt);
thumb_opcode th_vcvt_float_to_double(uint32_t vd, uint32_t vm);
thumb_opcode th_vcvt_double_to_float(uint32_t vd, uint32_t vm);
thumb_opcode th_vcvt_fp_int(uint32_t vd, uint32_t vm, uint32_t opc, uint32_t sz,
                            uint32_t op);

thumb_opcode th_it(uint16_t condition, uint16_t mask);

thumb_opcode th_svc(uint32_t imm);