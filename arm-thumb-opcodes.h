/*
 *  ARMvX-m opcodes for TCC 
 *  Uses thumb instruction set
 * 
 *  Based on: 
 *  ARM Thumb 2 instruction functions for TCC
 *  Copyright (c) 2020 Erlend J. Sveen  
 *  from: https://git.erlendjs.no/erlendjs/tinycc/-/blob/arm-thumb/arm-thumb-gen.c
 *        https://git.erlendjs.no/erlendjs/tinycc/-/blob/arm-thumb/arm-thumb-instructions.c
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

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#ifndef TCC_DEBUG
#define TCC_DEBUG 0 
#endif 

#define TRACE(...) 
#define LOG(...)

#if TCC_DEBUG == 1 || TCC_DEBUG == 2
#undef LOG
#define LOG(...) printf("[INF]: ");printf(__VA_ARGS__);printf("\n")
#endif 

#if TCC_DEBUG == 2
#undef TRACE
#define TRACE(...) printf("[TRC]: ");printf(__VA_ARGS__);printf("\n")
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
    FLAGS_BEHAVIOUR_NOT_IMPORANT = 0,
    FLAGS_BEHAVIOUR_SET = 1,
    FLAGS_BEHAVIOUR_BLOCK = 2,
} flags_behaviour;

typedef struct thumb_opcode {
    uint8_t size;
    uint32_t opcode;
} thumb_opcode;

uint32_t th_packimm_10_11_0(uint32_t imm);
uint32_t th_pack_const(uint32_t imm);
uint32_t th_encbranch_b_t3(uint32_t imm);

void th_nop();

void th_bx_reg(uint16_t rm);
void th_bl_t1(uint32_t imm);
void th_blx_reg(uint16_t rm);
thumb_opcode th_b_t1(uint16_t cond, uint16_t imm8);
void th_b_t2(int16_t imm11);
void th_b_t3(uint16_t op, uint32_t imm);
void th_b_t4(int32_t imm);

void th_mov_reg(uint16_t rd, uint16_t rm);
int th_mov_imm(uint16_t rd, uint16_t imm);

int th_generic_op_imm_with_status(uint16_t op, uint16_t rd, uint16_t rn, uint32_t imm, flags_behaviour setflags);
int th_generic_op_imm(uint16_t op, uint16_t rd, uint16_t rn, uint32_t imm);

thumb_opcode th_add_reg(uint16_t rd, uint16_t rn, uint16_t rm);
int th_add_imm(uint16_t rd, uint16_t rn, uint32_t imm);
int th_bic_imm(uint16_t rd, uint16_t rn, uint32_t imm);
int th_and_imm(uint16_t rd, uint16_t rn, uint32_t imm);
void th_and_reg(uint16_t rd, uint16_t rn, uint16_t rm);
int th_xor_reg(uint16_t rd, uint16_t rn, uint16_t rm);
int th_xor_imm(uint16_t rd, uint16_t rn, uint32_t imm);
void th_rsb_reg(uint16_t rd, uint16_t rn, uint16_t rm);
void th_sub_reg(uint16_t rd, uint16_t rn, uint16_t rm);
void th_adc_reg(uint16_t rd, uint16_t rn, uint16_t rm);
int th_adc_imm(uint16_t rd, uint16_t rn, uint32_t imm);
int th_sbc_imm(uint16_t rd, uint16_t rn, uint32_t imm);
int th_orr_imm(uint16_t rd, uint16_t rn, uint32_t imm);
void th_sbc_reg(uint16_t rd, uint16_t rn, uint16_t rm);
void th_cmp_reg(uint16_t rn, uint16_t rm);
void th_orr_reg(uint16_t rd, uint16_t rn, uint16_t rm);
int th_sub_imm(uint16_t rd, uint16_t rn, uint32_t imm);



thumb_opcode th_push(uint16_t regs);
int th_ldr_literal_estimate(uint16_t rt, uint16_t imm);
thumb_opcode th_ldrsh_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw);
void th_ldrsh_reg(uint32_t rt, uint32_t rn, uint32_t rm);
thumb_opcode th_ldrh_imm(uint16_t rt, uint16_t rn, uint16_t imm, uint16_t puw);
void th_ldrh_reg(uint32_t rt, uint32_t rn, uint32_t rm);
thumb_opcode th_ldrsb_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw);
void th_ldrsb_reg(uint32_t rt, uint32_t rn, uint32_t rm);
thumb_opcode th_ldrb_imm(uint16_t rt, uint16_t rn, uint16_t imm, uint16_t puw);
void th_ldrb_reg(uint32_t rt, uint32_t rn, uint32_t rm);
thumb_opcode th_ldr_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw);
void th_ldr_reg(uint32_t rt, uint32_t rn, uint32_t rm);
thumb_opcode th_ldr_literal(uint16_t rt, uint16_t imm, uint16_t add);

thumb_opcode th_pop(uint16_t regs);
int th_strh_imm(uint16_t rt, uint16_t rn, uint16_t imm, uint16_t puw);
void th_strh_reg(uint32_t rt, uint32_t rn, uint32_t rm);
int th_strb_imm(uint16_t rt, uint16_t rn, uint16_t imm, uint16_t puw);
void th_strb_reg(uint32_t rt, uint32_t rn, uint32_t rm);
int th_str_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw);
void th_str_reg(uint32_t rt, uint32_t rn, uint32_t rm);

void th_mul(uint16_t rd, uint16_t rn, uint16_t rm);
void th_umull(uint32_t rdlo, uint16_t rdhi, uint16_t rn, uint16_t rm);
void th_udiv(uint16_t rd, uint16_t rn, uint16_t rm);
void th_sdiv(uint16_t rd, uint16_t rn, uint16_t rm);

void th_add_sp_imm(uint16_t rd, uint16_t imm);
int th_rsb_imm(uint16_t rd, uint16_t rn, uint16_t imm, flags_behaviour setflags);
int th_shift_armv7m(uint16_t rd, uint16_t rm, uint16_t imm, uint16_t type);
int th_lsl_reg(uint16_t rd, uint16_t rn, uint16_t rm);
int th_lsl_imm(uint16_t rd, uint16_t rm, uint16_t imm);
int th_lsr_imm(uint16_t rd, uint16_t rm, uint16_t imm);
int th_asr_reg(uint16_t rd, uint16_t rn, uint16_t rm);
int th_asr_imm(uint16_t rd, uint16_t rm, uint16_t imm);

thumb_opcode th_cmp_imm(uint16_t rm, uint16_t imm);

void th_vpush(uint32_t regs);
void th_vpop(uint32_t regs);
void th_vmov_register(uint16_t vd, uint16_t vm);
void th_vldr(uint32_t rn, uint32_t vd, uint32_t add, uint32_t is_doubleword, uint32_t imm);
void th_vstr(uint32_t rn, uint32_t vd, uint32_t add, uint32_t is_doubleword, uint32_t imm);
void th_vmov_gp_sp(uint16_t rt, uint16_t sn, uint16_t to_arm_register);
void th_vmov_2gp_dp(uint16_t rt, uint16_t rt2, uint16_t dm, uint16_t to_arm_register);
uint32_t gen_th_sub_sp_imm(uint16_t rd, uint32_t imm);
uint32_t th_sub_sp_imm_estimate(uint16_t rd, uint32_t imm);
void th_sub_sp_imm(uint16_t rd, uint16_t imm);
void th_vmrs(uint16_t rt);
void th_vcvt_float_to_double(uint32_t vd, uint32_t vm);
void th_vcvt_double_to_float(uint32_t vd, uint32_t vm);
void th_vcvt_fp_int(uint32_t vd, uint32_t vm, uint32_t opc, uint32_t sz, uint32_t op);