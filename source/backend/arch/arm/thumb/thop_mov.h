#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_mov_reg(uint32_t rd, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding, bool in_it);

thumb_opcode th_mov_imm(uint16_t rd, uint32_t imm, thumb_flags_behaviour setflags, thumb_enforce_encoding encoding);

thumb_opcode th_movt(uint32_t rd, uint32_t imm16);

thumb_opcode th_mov_reg_shift(uint32_t rd, uint32_t rm, uint32_t rs, thumb_flags_behaviour flags, thumb_shift shift,
                              thumb_enforce_encoding encoding);
