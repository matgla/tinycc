#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_push(uint32_t regs);
thumb_opcode th_pop(uint16_t regs);
thumb_opcode th_ldm(uint32_t rn, uint32_t regset, uint32_t writeback, thumb_enforce_encoding encoding);
thumb_opcode th_stm(uint32_t rn, uint32_t regset, uint32_t writeback, thumb_enforce_encoding encoding);
thumb_opcode th_ldmdb(uint32_t rn, uint32_t reglist, uint32_t w);
thumb_opcode th_stmdb(uint32_t rn, uint32_t reglist, uint32_t w, thumb_enforce_encoding encoding);
