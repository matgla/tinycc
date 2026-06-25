#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_nop(thumb_enforce_encoding encoding);
thumb_opcode th_sev(thumb_enforce_encoding encoding);
thumb_opcode th_wfe(thumb_enforce_encoding encoding);
thumb_opcode th_wfi(thumb_enforce_encoding encoding);
thumb_opcode th_yield(thumb_enforce_encoding encoding);
thumb_opcode th_svc(uint32_t imm);
thumb_opcode th_bkpt(uint32_t imm);
thumb_opcode th_udf(uint32_t imm, thumb_enforce_encoding encoding);
thumb_opcode th_cps(uint32_t enable, uint32_t i, uint32_t f);
thumb_opcode th_clrex();
thumb_opcode th_csdb();
thumb_opcode th_dmb(uint32_t option);
thumb_opcode th_dsb(uint32_t option);
thumb_opcode th_isb(uint32_t option);
thumb_opcode th_ssbb();
thumb_opcode th_it(uint16_t cond, uint16_t mask);
thumb_opcode th_clz(uint32_t rd, uint32_t rm);
