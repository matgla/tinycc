#pragma once

#include <stdint.h>

#include "thumb.h"

/* Generic ARM coprocessor register-transfer / data-processing instructions.
 *
 * On ARMv8-M these are the only way to reach a vendor coprocessor attached to
 * the CDE/coprocessor port.  RP2350 puts its GPIO coprocessor on p0, the
 * double coprocessor (DCP) on p4/p5 and the redundancy coprocessor on p7.
 *
 * Every form has a base encoding and a "2" encoding (`cdp2`, `mcr2`, ...).
 * They differ only in the top nibble of the first halfword; the `two` argument
 * selects between them.  On the DCP the "2" forms are the non-engaging "peek"
 * reads used by state save/restore, so the distinction is load-bearing.
 *
 * `MRC`/`MRC2` with rt == R_PC (0b1111) writes the condition flags rather than
 * a general register — that is the APSR_nzcv form, and it needs no special
 * casing here beyond passing rt = 15.
 */

thumb_opcode th_cdp(uint32_t coproc, uint32_t opc1, uint32_t crd, uint32_t crn, uint32_t crm, uint32_t opc2,
                    uint32_t two);

thumb_opcode th_mcr(uint32_t coproc, uint32_t opc1, uint32_t rt, uint32_t crn, uint32_t crm, uint32_t opc2,
                    uint32_t two);
thumb_opcode th_mrc(uint32_t coproc, uint32_t opc1, uint32_t rt, uint32_t crn, uint32_t crm, uint32_t opc2,
                    uint32_t two);

thumb_opcode th_mcrr(uint32_t coproc, uint32_t opc1, uint32_t rt, uint32_t rt2, uint32_t crm, uint32_t two);
thumb_opcode th_mrrc(uint32_t coproc, uint32_t opc1, uint32_t rt, uint32_t rt2, uint32_t crm, uint32_t two);
