#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_ldrd_imm(uint32_t rt, uint32_t rt2, uint32_t rn, int imm, uint32_t puw);
thumb_opcode th_strd_imm(uint32_t rt, uint32_t rt2, uint32_t rn, int imm, uint32_t puw);
