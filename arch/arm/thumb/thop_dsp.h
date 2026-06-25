#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_uadd8(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_usub8(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_sel(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_pkhbt(uint32_t rd, uint32_t rn, uint32_t rm, thumb_shift shift);
