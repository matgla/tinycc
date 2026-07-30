#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_ldr_literal(uint16_t rt, uint32_t imm, uint32_t add);
