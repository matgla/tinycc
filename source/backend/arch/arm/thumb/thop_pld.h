#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_pld_literal(int imm);
thumb_opcode th_pld_imm(uint32_t rn, uint32_t w, int imm);
thumb_opcode th_pld_reg(uint32_t rn, uint32_t rm, uint32_t w, thumb_shift shift);
thumb_opcode th_pli_literal(int imm);
thumb_opcode th_pli_imm(uint32_t rn, uint32_t w, int imm);
thumb_opcode th_pli_reg(uint32_t rn, uint32_t rm, uint32_t w, thumb_shift shift);
