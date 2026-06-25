#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_tbb(uint32_t rn, uint32_t rm, uint32_t h);
thumb_opcode th_tt(uint32_t rd, uint32_t rn, uint32_t a, uint32_t t);
