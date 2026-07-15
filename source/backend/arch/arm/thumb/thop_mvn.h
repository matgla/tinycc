#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_mvn_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding);
thumb_opcode th_mvn_imm(uint32_t rd, uint32_t rm, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding);
