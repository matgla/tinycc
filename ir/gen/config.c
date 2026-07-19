/*
 *  TCC IR - Operation Configuration Table
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

// clang-format off
const IRRegistersConfig irop_config[] = {
    [TCCIR_OP_ADD] = {1, 1, 1},
    [TCCIR_OP_ADC_USE] = {1, 1, 1},
    [TCCIR_OP_ADC_GEN] = {1, 1, 1},
    [TCCIR_OP_SUB] = {1, 1, 1},
    [TCCIR_OP_SUBC_GEN] = {1, 1, 1},
    [TCCIR_OP_SUBC_USE] = {1, 1, 1},
    [TCCIR_OP_MUL] = {1, 1, 1},
    [TCCIR_OP_MLA] = {1, 1, 1},  /* MLA has accumulator as extra operand at pool[operand_base+3] */
    [TCCIR_OP_UMULL] = {1, 1, 1},
    [TCCIR_OP_SMULL] = {1, 1, 1},
    [TCCIR_OP_DIV] = {1, 1, 1},
    [TCCIR_OP_UMOD] = {1, 1, 1},
    [TCCIR_OP_IMOD] = {1, 1, 1},
    [TCCIR_OP_AND] = {1, 1, 1},
    [TCCIR_OP_OR] = {1, 1, 1},
    [TCCIR_OP_XOR] = {1, 1, 1},
    [TCCIR_OP_SHL] = {1, 1, 1},
    [TCCIR_OP_SAR] = {1, 1, 1},
    [TCCIR_OP_SHR] = {1, 1, 1},
    [TCCIR_OP_PDIV] = {1, 1, 1},
    [TCCIR_OP_UDIV] = {1, 1, 1},
    [TCCIR_OP_CMP] = {0, 1, 1},
    [TCCIR_OP_RETURNVOID] = {0, 0, 0},
    [TCCIR_OP_RETURNVALUE] = {0, 1, 0},
    [TCCIR_OP_JUMP] = {1, 0, 0},
    [TCCIR_OP_JUMPIF] = {1, 1, 0},
    [TCCIR_OP_IJUMP] = {0, 1, 0},
    [TCCIR_OP_SETIF] = {1, 1, 0},
    /* FUNCPARAMVOID carries call_id in src2.c.i (encoded like FUNCPARAMVAL). */
    [TCCIR_OP_FUNCPARAMVOID] = {0, 0, 1},
    [TCCIR_OP_FUNCPARAMVAL] = {0, 1, 1},
    /* FUNCCALL* carries call_id in src2.c.i so backends can match parameters. */
    [TCCIR_OP_FUNCCALLVOID] = {0, 1, 1},
    [TCCIR_OP_FUNCCALLVAL] = {1, 1, 1},
    [TCCIR_OP_LOAD] = {1, 1, 0},
    [TCCIR_OP_STORE] = {1, 1, 0},
    [TCCIR_OP_ASSIGN] = {1, 1, 0},
    [TCCIR_OP_LEA] = {1, 1, 0},    /* dest = &src1 */
    [TCCIR_OP_LOAD_INDEXED] = {1, 1, 1},   /* dest = *(base + (index << scale)) */
    [TCCIR_OP_STORE_INDEXED] = {1, 1, 1},  /* *(base + (index << scale)) = src */
    [TCCIR_OP_LOAD_POSTINC] = {1, 1, 0},   /* dest = *ptr; ptr += offset */
    [TCCIR_OP_STORE_POSTINC] = {1, 1, 0},  /* *ptr = src; ptr += offset */
    [TCCIR_OP_TEST_ZERO] = {0, 1, 0},
    [TCCIR_OP_UBFX] = {1, 1, 1},  /* dest = (src1 >> lsb) & ((1<<width)-1); src2 = lsb|(width<<5) */
    [TCCIR_OP_SBFX] = {1, 1, 1},  /* dest = sign-extend(src1 field[lsb,width]); src2 = lsb|(width<<5) */
    [TCCIR_OP_BFI] = {1, 1, 1},   /* dest = src1 w/ field[lsb,width] := src2; lsb/width in bfi_params[] */
    /* Floating point operations */
    [TCCIR_OP_FADD] = {1, 1, 1}, [TCCIR_OP_FSUB] = {1, 1, 1}, [TCCIR_OP_FMUL] = {1, 1, 1}, [TCCIR_OP_FDIV] = {1, 1, 1},
    [TCCIR_OP_FNEG] = {1, 1, 0}, /* unary: src1=input, dest */
    [TCCIR_OP_FCMP] = {0, 1, 1},
    /* Floating point conversion operations */
    [TCCIR_OP_CVT_FTOF] = {1, 1, 0}, /* dest=result, src1=input */
    [TCCIR_OP_CVT_ITOF] = {1, 1, 0}, /* dest=result, src1=input */
    [TCCIR_OP_CVT_FTOI] = {1, 1, 0}, /* dest=result, src1=input */
    [TCCIR_OP_ZEXT] = {1, 1, 0},     /* dest = (u_dest_width) src1 */
    [TCCIR_OP_PACK64] = {1, 1, 1},   /* dest_lo = src1, dest_hi = src2 */
    /* Logical boolean operations */
    [TCCIR_OP_BOOL_OR] = {1, 1, 1},  /* dest = (src1 || src2) */
    [TCCIR_OP_BOOL_AND] = {1, 1, 1}, /* dest = (src1 && src2) */

    /* VLA / dynamic stack ops */
    [TCCIR_OP_VLA_ALLOC] = {0, 1, 1},      /* src1=size(bytes), src2=align(bytes) */
    [TCCIR_OP_VLA_SP_SAVE] = {1, 0, 0},    /* dest=stack slot to store SP */
    [TCCIR_OP_VLA_SP_RESTORE] = {0, 1, 0}, /* src1=stack slot holding saved SP */

    /* Inline asm markers/barrier.
     * INLINE_ASM carries inline_asm_id in src1.c.i. */
    [TCCIR_OP_ASM_INPUT] = {0, 1, 0}, [TCCIR_OP_INLINE_ASM] = {0, 1, 0}, [TCCIR_OP_ASM_OUTPUT] = {1, 0, 0},
    /* Explicit call sequence ops (Option A scaffold)
     * - CALLSEQ_BEGIN: src1=stack_size (bytes), src2=pad (bytes)
     * - CALLARG_REG: src1=value, src2=reg_index (immediate)
     * - CALLARG_STACK: src1=value, src2=stack_off (immediate)
     * - CALLSEQ_END: src1=stack_size (bytes), src2=pad (bytes)
     */
    [TCCIR_OP_CALLSEQ_BEGIN] = {0, 1, 1}, [TCCIR_OP_CALLARG_REG] = {0, 1, 1}, [TCCIR_OP_CALLARG_STACK] = {0, 1, 1},
    [TCCIR_OP_CALLSEQ_END] = {0, 1, 1},

    /* Init chain slot: src1 carries the chain slot symbol (SYMREF), no vreg */
    [TCCIR_OP_INIT_CHAIN_SLOT] = {0, 1, 0},
    /* No-operation */
    [TCCIR_OP_NOP] = {0, 0, 0},
    /* Prefetch: src1=address vreg, src2=rw hint (in c.i), no dest */
    [TCCIR_OP_PREFETCH] = {0, 1, 1},
    /* Trap instruction: no operands, no dest */
    [TCCIR_OP_TRAP] = {0, 0, 0},
    /* Setjmp: dest=return value (0 or 1), src1=buffer pointer vreg,
     * src2=address of the hidden r4-r11 save area (frame slot) */
    [TCCIR_OP_SETJMP] = {1, 1, 1},
    /* Longjmp: src1=buffer pointer vreg, no dest (does not return) */
    [TCCIR_OP_LONGJMP] = {0, 1, 0},
    /* Non-local goto setjmp/longjmp: full callee-saved save/restore (40-byte buffer) */
    [TCCIR_OP_NL_SETJMP] = {1, 1, 0},
    [TCCIR_OP_NL_LONGJMP] = {0, 1, 0},
    /* Jump table switch: src1=index vreg, src2=table_id, no dest */
    [TCCIR_OP_SWITCH_TABLE] = {0, 1, 1},
    /* Data-table switch load: dest=loaded value, src1=index, src2=value_table_id */
    [TCCIR_OP_SWITCH_LOAD] = {1, 1, 1},
    /* __builtin_apply_args: dest=pointer to saved arg block, no sources */
    [TCCIR_OP_BUILTIN_APPLY_ARGS] = {1, 0, 0},
    /* __builtin_apply: dest=return value, src1=fn_ptr, src2=args_block_ptr */
    [TCCIR_OP_BUILTIN_APPLY] = {1, 1, 1},
    /* __builtin_return: src1=result_ptr, no dest (does not return) */
    [TCCIR_OP_BUILTIN_RETURN] = {0, 1, 0},
    /* Block copy: dest=stack dest, src1=symbol src, src2=size */
    [TCCIR_OP_BLOCK_COPY] = {1, 1, 1},
    /* SELECT: dest=result, src1=then_val, src2=else_val, pool[+3]=condition */
    [TCCIR_OP_SELECT] = {1, 1, 1},
    [TCCIR_OP_ROR] = {1, 1, 1},
    /* Single-operand bit manipulation: dest = <op>(src1) */
    [TCCIR_OP_CLZ] = {1, 1, 0},
    [TCCIR_OP_RBIT] = {1, 1, 0},
    [TCCIR_OP_REV] = {1, 1, 0},
    [TCCIR_OP_REV16] = {1, 1, 0},
}
;
// clang-format on
