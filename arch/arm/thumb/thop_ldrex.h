#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_ldrex(uint32_t rt, uint32_t rn, int imm);
thumb_opcode th_strex(uint32_t rd, uint32_t rt, uint32_t rn, int imm);
thumb_opcode th_ldrexb(uint32_t rt, uint32_t rn);
thumb_opcode th_ldrexh(uint32_t rt, uint32_t rn);
thumb_opcode th_strexb(uint32_t rd, uint32_t rt, uint32_t rn);
thumb_opcode th_strexh(uint32_t rd, uint32_t rt, uint32_t rn);
