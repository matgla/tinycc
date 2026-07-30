#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_bx_reg(uint16_t rm);
thumb_opcode th_bl_t1(uint32_t imm);
thumb_opcode th_blx_reg(uint16_t rm);
thumb_opcode th_b_t1(uint32_t cond, uint32_t imm);
thumb_opcode th_b_t3(uint32_t cond, uint32_t imm);
thumb_opcode th_b_t4(int32_t imm);
thumb_opcode th_b_t2(int32_t imm11);
thumb_opcode th_cbz(uint16_t rn, uint32_t imm, uint32_t nonzero);
