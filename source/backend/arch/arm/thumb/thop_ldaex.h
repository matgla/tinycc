#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_ldaex(uint32_t rt, uint32_t rn);
thumb_opcode th_stlex(uint32_t rd, uint32_t rt, uint32_t rn);
thumb_opcode th_ldaexb(uint32_t rt, uint32_t rn);
thumb_opcode th_ldaexh(uint32_t rt, uint32_t rn);
thumb_opcode th_stlexb(uint32_t rd, uint32_t rt, uint32_t rn);
thumb_opcode th_stlexh(uint32_t rd, uint32_t rt, uint32_t rn);
