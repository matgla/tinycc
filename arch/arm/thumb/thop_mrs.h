#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_mrs(uint32_t rd, uint32_t sysm);
thumb_opcode th_msr(uint32_t specreg, uint32_t rn, uint32_t mask);
