#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_mul(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags,
                    thumb_enforce_encoding encoding);
thumb_opcode th_mla(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra);
thumb_opcode th_mls(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra);
thumb_opcode th_umull(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);
thumb_opcode th_umlal(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);
thumb_opcode th_smull(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);
thumb_opcode th_smlal(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);
thumb_opcode th_udiv(uint16_t rd, uint16_t rn, uint16_t rm);
thumb_opcode th_sdiv(uint16_t rd, uint16_t rn, uint16_t rm);
