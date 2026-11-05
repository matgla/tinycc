/*
 * On-device IR layout limits.
 *
 * Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

/* The on-device build keeps this in half the space: it is the most contended
 * array of a compile. 16-bit indexes limit one function to 64K instructions and
 * 64K operand slots, about 20K instructions in practice, which is also about what
 * 8 MB of PSRAM can hold of one function. tcc_error() says so past that.
 * Plain fields, not bit-fields: the instruction count of a compile rose 7.8% with
 * a 20-bit operand_base, more than the misses it saved on most files. */
#if defined(__thumb__) || defined(TCC_IR_SMALL)
#define TCC_IR_SMALL_QUAD 1
#define TCC_IR_ORIG_INDEX_MAX 0xFFFF
#define TCC_IR_LINE_MAX 0xFFFF
#define TCC_IR_POOL_MAX_SLOTS 0x10000
#else
#define TCC_IR_SMALL_QUAD 0
#define TCC_IR_ORIG_INDEX_MAX 0x7FFFFFFF
#define TCC_IR_LINE_MAX 0xFFFFFFF
#define TCC_IR_POOL_MAX_SLOTS 0x40000000
#endif

/* Instruction and block indexes in the on-device CFG: a block index is signed, for
 * the -1 of "no block", and tcc_error() past that. */
#if TCC_IR_SMALL_QUAD
#include <stdint.h>
typedef uint16_t ir_insn_idx_t;
typedef int16_t ir_block_idx_t;
#define TCC_IR_MAX_BLOCKS 0x7FFF
#else
typedef int ir_insn_idx_t;
typedef int ir_block_idx_t;
#define TCC_IR_MAX_BLOCKS 0x7FFFFFFF
#endif
