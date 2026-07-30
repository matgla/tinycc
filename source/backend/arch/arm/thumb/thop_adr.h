#pragma once

#include <stdint.h>

#include "thumb.h"

thumb_opcode th_adr_imm(uint32_t rd, int imm, thumb_enforce_encoding encoding);
