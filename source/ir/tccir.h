/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#pragma once

#include <stdint.h>

#include "tccir_operand.h"
#include "tccls.h"

#define PREG_SPILLED 0x20
#define PREG_NONE 0x1F     /* pr0/pr1 not allocated - just the register bits */
#define PREG_REG_NONE 0x1F /* pr0_reg/pr1_reg not allocated (5-bit field: 31) */

typedef enum TccIrOp
{
  TCCIR_OP_ADD,
  TCCIR_OP_ADC_USE,
  TCCIR_OP_ADC_GEN,
  TCCIR_OP_SUB,
  TCCIR_OP_SUBC_USE,
  TCCIR_OP_SUBC_GEN,
  TCCIR_OP_MUL,
  TCCIR_OP_MLA, /* Multiply-Accumulate: dest = src1 * src2 + accum */
  TCCIR_OP_UMULL,
  TCCIR_OP_DIV,
  TCCIR_OP_UMOD,
  TCCIR_OP_IMOD,
  TCCIR_OP_AND,
  TCCIR_OP_OR,
  TCCIR_OP_XOR,
  TCCIR_OP_SHL,
  TCCIR_OP_SAR,
  TCCIR_OP_SHR,
  TCCIR_OP_PDIV,
  TCCIR_OP_UDIV,
  TCCIR_OP_CMP,
  TCCIR_OP_RETURNVOID,
  TCCIR_OP_RETURNVALUE,
  TCCIR_OP_SET_CHAIN, /* Set static chain register before nested function call */
  TCCIR_OP_JUMP,
  TCCIR_OP_JUMPIF,
  /* Indirect jump (computed goto): target in src1 */
  TCCIR_OP_IJUMP,
  TCCIR_OP_SETIF,
  TCCIR_OP_TEST_ZERO,
  TCCIR_OP_FUNCPARAMVOID,
  TCCIR_OP_FUNCPARAMVAL,
  TCCIR_OP_FUNCCALLVOID,
  TCCIR_OP_FUNCCALLVAL,
  TCCIR_OP_LOAD,
  TCCIR_OP_STORE,
  TCCIR_OP_ASSIGN,
  TCCIR_OP_LEA, /* Load Effective Address: dest = &src1 (compute address without loading) */

  /* Indexed memory operations for array access optimization */
  TCCIR_OP_LOAD_INDEXED,  /* dest = *(base + (index << scale)) - ARM LDR rd,[rn,rm,LSL #scale] */
  TCCIR_OP_STORE_INDEXED, /* *(base + (index << scale)) = src - ARM STR rd,[rn,rm,LSL #scale] */

  /* Post-increment memory operations for sequential access optimization
   * These combine a load/store with pointer increment: */
  TCCIR_OP_LOAD_POSTINC,  /* dest = *ptr; ptr += offset - ARM LDR rd,[rn],#imm */
  TCCIR_OP_STORE_POSTINC, /* *ptr = src; ptr += offset - ARM STR rd,[rn],#imm */

  /* Unsigned bitfield extract: dest = (src1 >> lsb) & ((1<<width)-1)
   * src2 encodes lsb (bits 0-4) and width (bits 5-9): src2 = lsb | (width << 5)
   * ARM: UBFX Rd, Rn, #lsb, #width */
  TCCIR_OP_UBFX,

  /* Signed bitfield extract: dest = sign-extend of src1 bits [lsb..lsb+width-1].
   * src2 encodes lsb (bits 0-4) and width (bits 5-9): src2 = lsb | (width << 5).
   * The signed analog of UBFX, from the (x<<a)>>b arithmetic-shift idiom.
   * ARM: SBFX Rd, Rn, #lsb, #width */
  TCCIR_OP_SBFX,

  /* Bitfield insert: dest = (src1 with bits [lsb..lsb+width-1] replaced by the
   * low `width` bits of src2).  Algebraically == (src1 & ~field) | (src2 << lsb)
   * for field = ((1<<width)-1)<<lsb when src2 < 2^width.  lsb/width are carried
   * in ir->bfi_params[orig_index], not the operands (src1=host word, src2=value).
   * ARM: BFI Rd, Rn, #lsb, #width (Rd preset to the host word). */
  TCCIR_OP_BFI,

  /* Floating point operations */
  TCCIR_OP_FADD, /* float/double addition */
  TCCIR_OP_FSUB, /* float/double subtraction */
  TCCIR_OP_FMUL, /* float/double multiplication */
  TCCIR_OP_FDIV, /* float/double division */
  TCCIR_OP_FNEG, /* float/double negation */
  TCCIR_OP_FCMP, /* float/double comparison */
  /* Floating point conversion operations */
  TCCIR_OP_CVT_FTOF, /* float to double or double to float */
  TCCIR_OP_CVT_ITOF, /* int to float/double */
  TCCIR_OP_CVT_FTOI, /* float/double to int */
  /* Integer zero-extension: dest = (u_dest_width) src. Always zero-extends
   * regardless of source signedness — distinguished from ASSIGN/OR so the
   * optimizer never sign-extends the source value when folding. */
  TCCIR_OP_ZEXT,
  /* Pack two u32 values into a u64: dest_lo = src1, dest_hi = src2.
   * Emitted by a peephole that detects `((u64)hi << 32) | (u64)lo` chains
   * (ZEXT + SHL #32 + ZEXT + OR) and collapses them.  Backend lowers to
   * two 32-bit register moves; regalloc can often eliminate them. */
  TCCIR_OP_PACK64,
  /* Logical boolean operations - produce 0/1 result */
  TCCIR_OP_BOOL_OR,  /* (src1 != 0) || (src2 != 0) -> 0/1 */
  TCCIR_OP_BOOL_AND, /* (src1 != 0) && (src2 != 0) -> 0/1 */

  /* Variable-length array (VLA) / dynamic stack allocation */
  TCCIR_OP_VLA_ALLOC,      /* adjust SP by runtime size, with alignment */
  TCCIR_OP_VLA_SP_SAVE,    /* save current SP to a fixed stack slot */
  TCCIR_OP_VLA_SP_RESTORE, /* restore SP from a fixed stack slot */

  /* Inline asm support (IR-only):
   * - ASM_INPUT: marks vreg uses feeding the asm block
   * - INLINE_ASM: barrier/call-like instruction carrying asm payload id
   * - ASM_OUTPUT: marks vreg defs produced by the asm block
   */
  TCCIR_OP_ASM_INPUT,
  TCCIR_OP_INLINE_ASM,
  TCCIR_OP_ASM_OUTPUT,

  /* Explicit call sequence lowering (Option A scaffold).
   * These ops allow the IR to represent the ABI-mandated call argument
   * placement explicitly, so backends can become mostly "dumb emitters".
   *
   * Semantics (initially ARM/AAPCS-focused, but encoded generically):
   * - CALLSEQ_BEGIN: reserve outgoing argument stack area (and optional pad)
   * - CALLARG_REG:   place an argument value into a numbered ABI arg register
   * - CALLARG_STACK: place an argument value at outgoing stack offset
   * - CALLSEQ_END:   release outgoing argument stack area (and optional pad)
   */
  TCCIR_OP_CALLSEQ_BEGIN,
  TCCIR_OP_CALLARG_REG,
  TCCIR_OP_CALLARG_STACK,
  TCCIR_OP_CALLSEQ_END,

  /* Store parent FP (R7) into chain slot for nested function trampoline.
   * src1.c.i = ELF symbol index of the chain slot in .data */
  TCCIR_OP_INIT_CHAIN_SLOT,

  /* No-operation placeholder for dead instructions */
  TCCIR_OP_NOP,

  /* Prefetch data cache hint (PLD/PLI on ARM) - __builtin_prefetch */
  TCCIR_OP_PREFETCH,

  /* Generate a trap instruction (e.g., UDF on ARM) */
  TCCIR_OP_TRAP,

  /* Setjmp/longjmp for non-local exits:
   * SETJMP: src1 = jump buffer pointer, dest = return value (0 on first call, 1 on longjmp)
   * LONGJMP: src1 = jump buffer pointer, src2.c.i = return value (forced to 1)
   */
  TCCIR_OP_SETJMP,
  TCCIR_OP_LONGJMP,

  /* Non-local goto setjmp/longjmp: saves/restores ALL callee-saved registers
   * (r4-r11) plus SP and resume address in a 40-byte buffer.
   * Used for nested function non-local goto (__label__ + goto from nested func).
   * NL_SETJMP: src1 = jump buffer pointer (40 bytes), dest = return value
   * NL_LONGJMP: src1 = jump buffer pointer (40 bytes)
   */
  TCCIR_OP_NL_SETJMP,
  TCCIR_OP_NL_LONGJMP,

  /* __builtin_apply_args / __builtin_apply / __builtin_return support:
   * BUILTIN_APPLY_ARGS: dest = pointer to saved incoming arg registers (r0-r3)
   * BUILTIN_APPLY: dest = pointer to return-value block;
   *                src1 = function pointer, src2 = args block (from apply_args)
   * BUILTIN_RETURN: src1 = pointer to return-value block (from apply)
   */
  TCCIR_OP_BUILTIN_APPLY_ARGS,
  TCCIR_OP_BUILTIN_APPLY,
  TCCIR_OP_BUILTIN_RETURN,

  /* Jump table switch for dense case statements:
   * src1 = index vreg (already adjusted: value - min_case)
   * src2.c.i = table_id (references switch table data)
   * no dest - this instruction branches directly
   */
  TCCIR_OP_SWITCH_TABLE,

  /* Block copy from const data section to stack:
   * dest = STACKOFF destination (local stack offset, is_local=1)
   * src1 = SYMREF source (anonymous symbol in rodata section)
   * src2 = IMM32 size in bytes
   * No vreg uses/defs - operates on fixed stack locations and symbols.
   * Backend should generate LDM/STM for optimal ARM Thumb-2 code.
   */
  TCCIR_OP_BLOCK_COPY,

  /* Conditional select (if-then-else without branches):
   * dest = (condition) ? src1 : src2
   * dest = result vreg
   * src1 = "then" value (vreg, IMM32, or SYMREF)
   * src2 = "else" value (vreg, IMM32, or SYMREF)
   * pool[operand_base+3] = IMM32 condition code (ARM cond nibble 0-0xD)
   * Must be preceded by a CMP that sets condition flags.
   * Backend emits ITE cond; MOV/LDR dest, src1; MOV/LDR dest, src2.
   */
  TCCIR_OP_SELECT,
  TCCIR_OP_ROR,

  /* Data-table switch dispatch:
   * dest = vreg receiving the loaded value
   * src1 = index vreg (already adjusted: value - min_case, range-checked)
   * src2.c.i = switch_value_table_id
   * Loads values[src1] from the inline data table at codegen. Falls through
   * to the next instruction (typically a JMP to the merge block). The caller
   * is responsible for emitting a preceding range check that branches to a
   * separate block when the index is out of range; that block must load the
   * default_val from the value table (we keep range-check + default outside
   * SWITCH_LOAD itself to leverage existing CMP/JUMPIF/ASSIGN lowering).
   */
  TCCIR_OP_SWITCH_LOAD,
  /* Signed 32x32 -> 64 multiply: {dest_hi:dest_lo} = (int32)src1 * (int32)src2.
   * Placed at the end of the enum to avoid shifting other op values, which
   * could break ranges or generated tables that depend on absolute positions. */
  TCCIR_OP_SMULL,
  /* Single-operand bit manipulation: dest = <op>(src1), 32-bit only.
   * These map one-to-one onto Thumb-2 instructions and exist so the
   * __builtin_clz/ctz/bswap family does not have to go through a libcall.
   * They are only emitted when the active target advertises the encoding
   * (see tcc_machine_has_bit_ops). */
  TCCIR_OP_CLZ,   /* count leading zeros (clz);  clz(0) == 32 */
  TCCIR_OP_RBIT,  /* reverse bit order (rbit) */
  TCCIR_OP_REV,   /* reverse byte order in a word (rev)  == bswap32 */
  TCCIR_OP_REV16, /* reverse byte order in each halfword (rev16) */
  /* dest = this function's return address: __builtin_return_address(0).  Read
   * from the slot the prologue saved LR to, SP-relative, so it needs no frame
   * pointer.  No sources; its value depends on the call site, so a function
   * using it is never pure. */
  TCCIR_OP_RETURN_ADDRESS,
  /* UMAAL: u32(src1) * u32(src2) + lo32(accum) + hi32(accum); a word accumulator has high half zero. */
  TCCIR_OP_UMAAL,
  /* dest = fmin/fmax(src1, src2) on float/double: IEEE minNum/maxNum, a NaN
   * operand loses to a number.  @min/@max on floating point.  Inline
   * (vminnm.f32/vmaxnm.f32) where the FPU has_fminmax, else rewritten into a
   * call to fminf/fmin/fmaxf/fmax when it is put (ir/gen/softfloat.c). */
  TCCIR_OP_FMIN,
  TCCIR_OP_FMAX,
  /* Number of opcodes -- not an op.  Sizes ir_op_props[] (op_props.h), whose
   * selftest fails for any op below it that the table does not classify. */
  TCCIR_OP_COUNT
} TccIrOp;

/* Ops with a fourth (accumulator) operand at pool[operand_base+3] that is a
 * value read by the instruction. */
static inline int tcc_ir_op_is_mac(int op)
{
  return op == TCCIR_OP_MLA || op == TCCIR_OP_UMAAL;
}

/* Size (in bytes) at or above which the backend lowers a TCCIR_OP_BLOCK_COPY to
 * a real memcpy() call instead of an inline LDM/STM sequence (see
 * tcc_gen_machine_block_copy_mop in arm-thumb-gen.c).  A memcpy call clobbers
 * the caller-saved registers, so register allocation must treat a block copy of
 * at least this size as a call site (ra_build_call_prefix in ir/regalloc.c) and
 * force any value live across it off r0-r3/r12.  The inline path below this size
 * preserves everything it touches via scratch save/restore, so it is not a call.
 * The two sites must agree on this threshold; keep them in sync via this macro. */
#define TCCIR_BLOCK_COPY_MEMCPY_MIN_BYTES 64

/* FUNCPARAMVAL encoding helpers:
 * src2.c.i encodes both parameter index (lower 16 bits) and call_id (upper 16 bits)
 * This keeps call/param binding explicit and makes the IR more compact.
 */
#define TCCIR_ENCODE_PARAM(call_id, param_idx)                                                                         \
  ((int64_t)(int32_t)(((uint32_t)(call_id) << 16) | ((uint32_t)(param_idx) & 0xFFFFu)))
#define TCCIR_DECODE_CALL_ID(encoded) ((int)(((uint32_t)(encoded)) >> 16))
#define TCCIR_DECODE_PARAM_IDX(encoded) ((int)(((uint32_t)(encoded)) & 0xFFFF))

/* FUNCCALL encoding helpers:
 * For FUNCCALLVOID/FUNCCALLVAL, src2.c.i encodes call_id (bits 16-31) and argc (bits 0-15).
 * This allows the backend to know how many arguments to expect without scanning.
 */
#define TCCIR_ENCODE_CALL(call_id, argc)                                                                               \
  ((int64_t)(int32_t)(((uint32_t)(call_id) << 16) | ((uint32_t)(argc) & 0xFFFFu)))
#define TCCIR_DECODE_CALL_ARGC(encoded) ((int)(((uint32_t)(encoded)) & 0xFFFF))

typedef struct CType CType;
typedef struct SValue SValue;
typedef struct NestedFunc NestedFunc;
typedef struct AttributeDef AttributeDef;

#ifdef CONFIG_TCC_ASM
typedef struct ASMOperand ASMOperand;
typedef struct TCCIRInlineAsm
{
  char *asm_str;
  int asm_len;
  int must_subst;
  int nb_operands;
  int nb_outputs;
  int nb_labels;
  uint8_t clobber_regs[NB_ASM_REGS];
  ASMOperand *operands; /* length (nb_operands + nb_labels) */
  SValue *values;       /* length nb_operands; operands[i].vt points into this */
} TCCIRInlineAsm;
#endif

/* TACQuadruple: Expanded instruction form for migration compatibility.
 * Used by tcc_ir_expand_quad() / tcc_ir_writeback_quad() during the transition
 * from embedded SValues to pool-based storage.
 */
typedef struct TACQuadruple
{
  int orig_index; /* Original IR index (stable across DCE) */
  TccIrOp op;     /* Operation code */
  SValue dest;    /* Destination operand */
  SValue src1;    /* First source operand */
  SValue src2;    /* Second source operand */
  int line_num;   /* Source line for debug info */
} TACQuadruple;

typedef struct Sym Sym;

/* Sentinel value indicating an interval hasn't started yet.
 * Using 0xFFFFFFFF since instruction indices are non-negative. */
#define INTERVAL_NOT_STARTED 0xFFFFFFFF

typedef struct IRVregReplacement
{
  uint8_t r0; // first physical register (PREG_* bits fit in a byte; LS's -1 is 0xFF)
  uint8_t r1; // second physical register (for long long)
  int offset; // stack offset if spilled
} IRVregReplacement;

typedef struct IRLiveInterval
{
  uint8_t addrtaken : 1;       // whether the variable's address is taken
  uint8_t is_float : 1;        // whether this is a float/double variable
  uint8_t is_double : 1;       // whether this is a double (vs float)
  uint8_t is_llong : 1;        // whether this is a long long (64-bit int)
  uint8_t is_complex : 1;      // Phase 3: whether this is a complex type
  uint8_t use_vfp : 1;         // whether to use VFP registers (hard float)
  uint8_t is_lvalue : 1;
  uint8_t is_volatile : 1;  // whether the source object has volatile-qualified type
  uint8_t crosses_call : 1; // whether interval spans a function call
  uint8_t phi_pinned : 1;   // register relied upon by identity phi — do not reassign
  uint8_t incoming_stack : 1; // param the ABI placed wholly on the caller's stack (incoming_reg0 < 0 is also "unset")
  uint8_t is_struct : 1;      // param of struct type: its vreg names memory, never a value
  uint8_t nested_home : 1;    // param captured by a nested function: it keeps a parent-frame home
  uint8_t no_remat : 1;       // reload when spilled, never rematerialise (-Os shared constant)
  /* The sixteen flags and the two incoming regs fill one word, and the
   * allocation's byte-wide registers sit in front of its offset: 24 bytes.
   * The per-function interval arrays (one entry per temp/var/param) are
   * alive through register allocation, at the compile's memory peak.
   * remat_kind has one kind; the stack-slot back-link is a list in
   * TCCStackLayout. */
  uint8_t remat_kind : 1;  // 0=none, 1=imm32: recompute value instead of reloading when spilled
  uint8_t loop_split : 1;  // ra:loop_split entry-copy temp: the split is the point, never coalesce it back with its source
  int8_t incoming_reg0;    // for params: which register arg arrives in (-1 if stack)
  int8_t incoming_reg1;    // for doubles: second register (-1 if not double or stack)
  IRVregReplacement allocation;
  uint32_t start;           // start instruction index
  uint32_t end;             // end instruction index
  int32_t original_offset; // for params: original offset from function entry point
  /* The constant of a remat_kind 1 temp is in TCCIRState.remat_imm: decided
   * after allocation, it took 4 bytes of every interval through every pass. */
} IRLiveInterval;

typedef struct IRCallArgument
{
  SValue value;    /* argument value as emitted in FUNCPARAMVAL */
  int instr_index; /* original FUNCPARAMVAL instruction index (for diagnostics) */
} IRCallArgument;

/* SpillCache: Track which registers hold which stack slot values.
 * Used to avoid redundant loads when value is already in a register after storeback.
 * Invalidated by: function calls, branches, stores to different offsets with same register.
 */
#define SPILL_CACHE_SIZE 8
typedef struct SpillCacheEntry
{
  int8_t valid;   // Whether this entry is valid
  int8_t reg;     // Register containing the value
  int32_t offset; // Stack offset (FP-relative)
} SpillCacheEntry;

typedef struct SpillCache
{
  SpillCacheEntry entries[SPILL_CACHE_SIZE];
  /* Tracks the most recently emitted spill helper to elide a redundant
   * LDR that immediately follows a STR (or LDR) to/from the same slot.
   * Only valid when ind == last_emit_ind (no intervening emission). */
  int last_emit_ind;
  int8_t last_emit_kind; /* 0=none, 1=STR, 2=LDR */
  int8_t last_emit_reg;
  int32_t last_emit_offset;
} SpillCache;

typedef enum TCCStackSlotKind
{
  TCC_STACK_SLOT_SPILL = 1,
  TCC_STACK_SLOT_PARAM_SPILL,
  TCC_STACK_SLOT_LOCAL,
  TCC_STACK_SLOT_VLA_SAVE,
} TCCStackSlotKind;

typedef struct TCCStackSlot
{
  TCCStackSlotKind kind;
  int vreg;      // primary owner vreg (or -1 for shared/fixed slots)
  int offset;    // frame-pointer relative offset (bytes)
  int size;      // slot size in bytes
  int alignment; // required alignment in bytes (power of two)
} TCCStackSlot;

typedef struct TCCStackLayout
{
  TCCStackSlot *slots;
  int slot_count;
  int slot_capacity;

  /* Optional fast index: frame offset -> slot index.
   * Uses open addressing with linear probing.
   * Empty keys are marked with INT32_MIN (see tccir.c implementation).
   */
  int *offset_hash_keys;
  int *offset_hash_values;
  int offset_hash_size; /* 0 if disabled, otherwise power-of-two */

  /* vreg -> slot for the stack-backed vregs, sorted by vreg (built by
   * tcc_ir_stack_build): kept here, not in every vreg's interval. */
  struct TCCStackVregSlot *vreg_slots;
  int vreg_slot_count;
} TCCStackLayout;

/* Switch table metadata for jump table generation */
typedef struct TCCIRSwitchTable
{
  int64_t min_val;     /* Minimum case value */
  int64_t max_val;     /* Maximum case value */
  int default_target;  /* IR index for default case */
  int *targets;        /* Array of IR indices [max-min+1] */
  int num_entries;     /* Size of targets array */
  int table_code_addr; /* Code address of start of table data (set during codegen) */
} TCCIRSwitchTable;

/* Switch value table: emitted when SWITCH_TABLE is rewritten to a data-table
 * load (TCCIR_OP_SWITCH_LOAD). Each case slot holds a value (IMM32 or SYMREF
 * address) that gets loaded into the destination vreg instead of dispatching
 * to a case body. SYMREF entries emit R_ARM_ABS32 relocations at the table's
 * rodata offset so the linker fills in the symbol's runtime address.
 *
 * The table itself lives in .rodata; rodata_sym is an anonymous symbol
 * pointing to the table's base.  The dispatch code loads rodata_sym into a
 * scratch register and uses an indexed shifted LDR to read values[index]. */
typedef struct TCCIRSwitchValueTable
{
  int num_entries;       /* Size of values[] (= num cases) */
  IROperand *values;     /* Per-case values (IMM32, SYMREF, etc.) */
  IROperand default_val; /* Out-of-range fallback value */
  Sym *rodata_sym;       /* Symbol pointing to table base in .rodata */
} TCCIRSwitchValueTable;

typedef struct TCCMachineScratchRegs
{
  unsigned char reg_count;
  unsigned char saved_mask;
  int regs[2];
} TCCMachineScratchRegs;

#define TCC_MACHINE_SCRATCH_NEEDS_PAIR (1u << 0)
#define TCC_MACHINE_SCRATCH_PREFERS_FLOAT (1u << 1)
#define TCC_MACHINE_SCRATCH_ALLOW_REUSE (1u << 2)
/* Exclude ABI arg registers (e.g. R0-R3 on ARM) from scratch allocation. */
#define TCC_MACHINE_SCRATCH_AVOID_CALL_ARG_REGS (1u << 3)
#include "small_ir.h"

/* Exclude "permanent scratch" regs (e.g. R11/R12 on ARM) from scratch allocation. */
#define TCC_MACHINE_SCRATCH_AVOID_PERM_SCRATCH (1u << 4)

typedef struct IRQuadCompact
{
#if TCC_IR_SMALL_QUAD
  uint16_t operand_base;
  uint8_t op;
#define IR_QUAD_FLAG uint8_t
#else
  int orig_index;               /* Original IR index (stable across DCE) */
  TccIrOp op;                   /* Operation code */
  uint32_t operand_base;        /* Index into svalue_pool */
  uint32_t line_num : 28;       /* Source line for debug info (non-negative, 28 bits = up to 268M lines) */
#define IR_QUAD_FLAG uint32_t
#endif
  /* TRAP only: __builtin_unreachable().  Emits no code, and a path that can
   * only end here may be assumed never taken (tcc_ir_opt_unreachable_fold).
   * Every other pass sees an ordinary TRAP -- a terminator with no successor
   * and a side effect -- which is the conservative reading. */
  IR_QUAD_FLAG unreachable : 1;
  IR_QUAD_FLAG is_jump_target : 1;  /* Set when at least one JUMP/JUMPIF targets this instruction */
  IR_QUAD_FLAG no_unroll : 1;       /* Set on rerolled back-edges to prevent re-unrolling */
  /* Emit a 2-byte NOP ahead of this instruction when the code offset is not
   * 4-aligned.  Measured on RP2350 silicon: a BACKWARD-branch target at
   * `addr % 4 == 2` costs about a cycle per iteration, because the M33 fetches
   * a word at a time and the first fetch after the refill then delivers half as
   * much.  It turned bench_memcpy's one-instruction-shorter checksum loop into
   * a 13.8% LOSS and handed it back in full (2,066,226 -> 1,812,226) once the
   * head was word-aligned.
   *
   * The head's WIDTH is irrelevant and must not be filtered on: 16-bit heads
   * benefit just as much (all six of mibench_sha's are `cmp`, worth -4.98% on
   * their own), and filtering to 32-bit heads took the rig set win from
   * -0.944% to -0.059%.
   *
   * Set by ssa:loop_bottom_test on the heads it makes bottom-tested, and by
   * tcc_ir_codegen_generate on every backward-branch target (TCC_ALIGN_LOOPS). */
  IR_QUAD_FLAG align_target : 1;
#if TCC_IR_SMALL_QUAD
  uint16_t orig_index;
  uint16_t line_num;
#endif
} IRQuadCompact;
#undef IR_QUAD_FLAG
#if TCC_IR_SMALL_QUAD
typedef char ir_quad_compact_is_8_bytes[sizeof(IRQuadCompact) == 8 ? 1 : -1];
#endif

/* Per-operation operand configuration (defined in tccir.c) */
typedef struct IRRegistersConfig
{
  uint8_t has_dest : 1;
  uint8_t has_src1 : 1;
  uint8_t has_src2 : 1;
} IRRegistersConfig;

extern const IRRegistersConfig irop_config[];

/* Forward declaration for FP materialization cache */
typedef struct TCCFPMatCache TCCFPMatCache;

/* How a parameter reaches the function body (pure-forwarder detection). */
typedef struct IRParamForm
{
  uint8_t kind;    /* IR_PF_* */
  int8_t reg_base; /* IR_PF_HOME: first argument register */
  int32_t vreg;    /* IR_PF_REG / IR_PF_MEM: the PARAM vreg */
  int32_t off;     /* IR_PF_MEM: parameter offset; IR_PF_HOME: home slot */
  int32_t words;   /* IR_PF_HOME: words stored into the home slot from registers */
  int32_t stack_off;   /* IR_PF_HOME: parameter offset of the words copied from the stack */
  int32_t stack_words; /* IR_PF_HOME: words copied from the stack, after the register words */
} IRParamForm;
#define IR_PF_NONE 0 /* unknown: never forwarded untouched */
#define IR_PF_REG 1  /* a value in the PARAM vreg */
#define IR_PF_MEM 2  /* memory at a parameter offset (stack scalar, in-place struct) */
#define IR_PF_HOME 3 /* a struct the prologue stores (and copies) into a home slot */

/* Words per ir->frame_objs record: [0] frame offset, [1] size incl. padding,
 * [2] size, [3] FRAME_OBJ_* flags | (scope end + 1) << 4, [4] scope start + 1
 * (0 = none; tcc_ir_frame_scope_block). */
#define FRAME_OBJ_WORDS 5

/* Where a nested function's capture ranges start (gen_function): the bottom
 * of the 16-bit frame offset space a struct operand can name.  They take no
 * room in the frame; a frame reaching down to them is refused. */
#define TCC_CAPTURE_SPACE (-32768 + 8)

/* -Wlifetime (lifetime.c): what the frontend knows about yasc ownership and
 * the raw IR does not.  An owned local or parameter, by its vreg. */
typedef struct IRLtVar
{
  int32_t vreg;
  int tok;            /* its name */
  int line;           /* where it is declared */
  uint8_t silent;     /* declared in an inlined body: its callee reports it */
  uint8_t untracked;  /* its address is taken: the analysis cannot follow it */
  uint8_t kind;       /* LTV_*; a plain pointer has none */
  uint8_t arg_report; /* an inlined callee's parameter, bound at a call to report */
  int16_t arg_index;  /* ... its argument number (from 0), -1 for any other variable */
  uint8_t yinit;      /* -W[maybe-]uninitialized: in yasc, where initialization is enforced (errors) */
  uint8_t yundef;     /* ... declared `= undefined`: its address may be taken before it has a value */
  uint8_t inout;      /* an `inout` parameter (a view whose target is not const): -Wparam-access; 2: a struct's */
  uint8_t outarg;     /* an `out` parameter, at its own level or the pointer it points at (`out T * maybe * view p`) */
  uint8_t implicit_view; /* a yasc local whose pointer says nothing: a view, never null (`* maybe` fix-it) */
  int arg_callee;     /* ... the callee's name (token), 0 when called through a pointer */
  int dealloc;        /* `owned(fclose)` as declared: the deallocator's name, 0 not said */
} IRLtVar;

/* yasc: an array, struct or union declared `= undefined` -- followed in its
   frame slot [off, off + size) until something is written to it */
typedef struct IRUnAgg
{
  int off, size;
  int tok;        /* its name */
  int line;       /* where it is declared */
  int idx;        /* the instruction its declaration comes before */
  uint8_t silent; /* declared in an inlined body */
} IRUnAgg;
#define LTV_OWNED 1 /* `T * owned`: followed for ownership */
#define LTV_MAYBE 2 /* `T * maybe`: followed for null */
#define LTV_OUT 4   /* `out T *`: the function must write *p, and not read it first */
#define LTV_MUT 8   /* points to memory that is not const: handed on, it may be written */
#define LTV_TAKEN 16 /* an owned parameter the function's result takes over (takes(p)) */
#define LTV_HANDLE 32 /* an int handle (`int owned(close) maybe fd`): "null" is < 0 */
#define LTV_OBJECT 64 /* an object owned as a whole (`regex_t owned(regfree) re`) */
#define LTV_RESULT 128 /* an inlined call's result variable (-O1+): maybe as the call's type says */

/* A `T * maybe` struct member reached through a pointer (`q->next`,
 * `(*q).next`): the temp that holds the member's address, its offset and its
 * name.  The checker follows each (base pointer, offset) as a variable. */
/* An owned pointer or int handle inside a local struct or array (`b.buf`,
 * `fds[0]`): it has no vreg, so the slot check (lifetime.c) follows it by
 * its frame offset. */
typedef struct IRLtSlot
{
  int32_t off;      /* its frame offset */
  int32_t base;     /* the local's frame offset and size: its address used */
  int32_t base_size; /* anywhere else stops the following of all its items */
  int dealloc;      /* owned(dealloc), 0 not said */
  uint8_t kind;     /* LTV_OWNED, LTV_MAYBE, LTV_HANDLE */
  /* an element of the innermost array it is in (`a[2]`, `b[1].buf`): the
     element's size (0 none), its index, the array's length, and where
     "[2]" is in the name -- a variable index (`a[i]`) is followed per array */
  uint8_t pre, suf;
  uint8_t silent; /* declared in an inlined body: its callee's to report */
  int16_t idx, nelem;
  int32_t esize;
  char name[40];
} IRLtSlot;

/* A local struct whose values borrow (`struct T borrows(text)`): followed
 * by -Wlifetime by frame offset, its pointer members' words in pmask */
typedef struct IRLtSobj
{
  int32_t off, size;
  int tok;
  uint8_t silent; /* declared in an inlined body */
  uint64_t pmask; /* bit w: the word at off + 4 * w holds a pointer */
} IRLtSobj;

#define LT_MEMBER_OWNED 1  /* IRLtMember.owned: owns its target */
#define LT_MEMBER_HANDLE 2 /* ... an int handle (`int owned(close) maybe fd`), not a pointer */
#define LT_MEMBER_NOTMAYBE 4 /* ... never null (`T * owned buf`): followed for ownership only */
typedef struct IRLtMember
{
  int32_t vreg;
  int offset;
  int tok;
  int owned; /* LT_MEMBER_*: `T * owned maybe` also followed for ownership; an int handle */
  int foff;  /* the member's offset in its struct (offset: from the address noted) */
  int esize; /* the struct's size: an element of an array of them (`p[1].next`) */
} IRLtMember;

/* Before instruction `idx`: a scope's end for an owned variable, or (var
 * LT_EV_RETURN) the jump there is a `return`. */
typedef struct IRLtEvent
{
  int32_t idx;
  int32_t var; /* index in lt_vars */
  int line;
} IRLtEvent;
#define LT_EV_RETURN (-1)
#define LT_EV_RESULT(v) (-2 - (v)) /* the inlined body whose result is variable v ends */

/* A call's parameters that take ownership, by source parameter (bit i). */
/* yasc tagged unions: a member access (tcc_ir_note_tagged): the member's
 * address -- a temp (vr), a frame offset (kind 1) or a symbol plus offset
 * (kind 2) -- before instruction `at` */
/* yasc register fields: a store to volatile memory with w1c bits (instructions
 * [from, to)): the w1c bits of each word from the destination's start, and
 * up to 16 fields (word, bits, name) for messages.  VREAD: instead, a copy
 * of a register into ordinary memory (a struct copy through memmove): the
 * local it fills holds what was read */
#define IR_W1C_WORDS 16
typedef struct IRW1cNote
{
  int32_t from, to;
  int line, nw, nf, vread;
  uint32_t wmask[IR_W1C_WORDS];
  uint8_t fword[IR_W1C_WORDS];
  uint32_t fbits[IR_W1C_WORDS];
  int ftok[IR_W1C_WORDS];
} IRW1cNote;

typedef struct IRTagNote
{
  int32_t at, vr, off;
  struct Sym *sym;
  int16_t tu, m; /* yasc_tagged[tu], its member m */
  int line;
  uint8_t kind; /* 0 temp, 1 frame offset, 2 symbol */
} IRTagNote;

/* IRLtCall.gkeep: a direct call to a prototype a system header gives, with
 * no function pointer parameter (qsort runs user code), writes only the
 * globals system headers declare (errno, stdout); a leaf callee cannot
 * reach this unit's static globals */
#define LT_GKEEP_SYS 1
#define LT_GKEEP_LEAF 2
typedef struct IRLtCall
{
  uint32_t owned;   /* `T * owned` parameters: the argument moves */
  uint32_t drop;    /* of them, `drop`: the callee frees it */
  uint8_t implicit; /* hidden arguments ahead of the first (struct return) */
  uint8_t known;     /* owned parameters recorded */
  uint8_t ret_maybe; /* the result is `T * maybe` */
  uint8_t in_inline; /* emitted inside an inlined body: its callee reports it */
  uint32_t nonnull;  /* pointer parameters declared in yasc without maybe: never null */
  uint32_t nonnull_attr; /* of them, __attribute__((nonnull)) from regular C */
  int callee;        /* the callee's name (token), 0 through a pointer */
  uint32_t keep;     /* parameters that cannot write through the argument: a
                        pointer to const, or no pointer at all */
  uint8_t gkeep;     /* which globals it cannot write (LT_GKEEP_*): all may change by default */
  uint8_t nparams;   /* declared parameters (at most 32); past them, varargs */
  uint8_t noted;     /* tcc_ir_lt_note_call saw the call */
  uint8_t galias;    /* an argument it may write through may point into a global: gkeep does not apply */
  uint32_t borrows;  /* a view result: the arguments it points into (borrows(...), the elision) */
  uint32_t sborrows; /* a borrowing struct's result (`struct T borrows(src)`): the arguments it borrows */
  uint32_t tborrow;  /* `out T * borrows(s) * view p` parameters: what the callee writes there points into s */
  uint32_t tbsrc;    /* ... the arguments their targets borrow from (all of them together) */
  int32_t ssize;     /* a borrowing struct's result: its size (256 at most), 0 none */
  uint64_t spmask;   /* ... the words of it that hold a pointer */
  int ret_dealloc;   /* an owned result's deallocator (owned(fclose), malloc(fclose, 1)), 0 not said */
  int drop_dealloc;  /* the first drop parameter's own owned(...), 0 not said: then the callee's name */
  uint32_t taken;    /* owned parameters the result takes over (takes(p)) */
  uint32_t handles;  /* int handle parameters: invalid is < 0, not 0 */
  uint32_t restricts; /* `T * restrict view` parameters: no other argument may point into the same object */
  uint32_t out;        /* `out T * p` parameters: the callee writes *p (yasc's enforced initialization) */
  uint32_t yread;      /* pointer parameters declared in yasc, not `out`: the callee may read *p */
  uint32_t outown;     /* parameters pointing at an owned pointer (getline's lineptr): the callee may replace it */
  uint32_t outown_out; /* of them, `out`: the callee writes one there without reading what was */
  uint32_t outown_maybe; /* of them, the owned pointer is maybe */
  uint32_t dropobj;  /* `drop T owned(release) * view p`: releases the object p points at */
  int dropobj_dealloc; /* the first such parameter's deallocator */
  uint32_t objout;   /* of outown, those that write an object owned as a whole (regcomp) */
  /* checked `T * view` parameters (a method's self): the callee keeps the
     owned members of what they point at valid -- it may replace them, never
     leave one freed -- so they must be valid when it is called */
  uint32_t vmem;
  /* `drop struct T * view self`: the callee releases those owned members */
  uint32_t dropmem;
} IRLtCall;

typedef struct TCCIRState
{
  // number of function parameters
  int8_t parameters_count;
  IRParamForm *param_forms; /* [parameters_count] */
  /* Named-argument usage for variadic prolog (AAPCS). */
  int named_arg_reg_bytes;
  int named_arg_stack_bytes;

  uint8_t is_variadic : 1;
  /* A long parameter straddles r3 and the stack: the prologue pushes r0-r3
   * below the stack arguments, as for a variadic function, so it lies
   * contiguous in memory and is used in place. */
  uint8_t push_arg_regs : 1;
  /* The body only forwards its parameters, untouched, to one call of the same
   * signature: emitted as a branch to it (tcc_ir_backend_fold_pure_forward). */
  uint8_t pure_forward : 1;
  uint8_t leaffunc : 1;
  uint8_t tail_call_only : 1;
  uint8_t naked : 1;
  /* -fstack-protector chose this function: a canary below the saved
   * registers (arm-thumb-frame.c), and no tail call */
  uint8_t ssp : 1;
  /* no canary whatever the body holds: no_stack_protector, or
   * -fstack-protector-explicit without stack_protect (frontend/gen/op/ubsan.c) */
  uint8_t ssp_off : 1;
  /* Set by noreturn_collapse when the body has been replaced with `b .`:
   * control never reaches the epilogue, so suppress emitting it (saves the
   * unreachable `bx lr` after the self-jump). Unlike `naked`, this does
   * NOT suppress the prologue or debug info — the collapsed function is
   * still a normal callee from the linker's perspective. */
  uint8_t noreturn : 1;
  uint8_t processing_if : 1;
  uint8_t check_for_backwards_jumps : 1;
  uint8_t basic_block_start : 1;
  uint8_t prevent_coalescing;
  uint8_t has_static_chain : 1;      /* function uses static chain for nested func */
  uint8_t needs_chain_save : 1;      /* must save chain at FP-4 for multi-hop child access */
  uint8_t emits_set_chain : 1;       /* parent writes R10 (SET_CHAIN/INIT_CHAIN_SLOT) to call a
                                        nested function; R10 is callee-saved, so it must be in
                                        the prologue save mask and excluded from reassignment */
  int32_t static_chain_vreg;         /* vreg holding static chain pointer (parent FP) */
  /* Hard-float, a result returned in s0..s(vfp_ret_words-1)
   * (gfunc_sret_vfp_words): the frame buffer the body's returns write and the
   * exit loads the registers from.  func_vc's slot holds its address, not a
   * hidden parameter. */
  int32_t vfp_ret_buf;
  uint8_t vfp_ret_words;
  int32_t captured_offsets_list[32]; /* offsets of captured vars (for chain-relative access) */
  int32_t captured_chain_depths[32]; /* 1 = direct R10, 2+ = multi-hop */
  /* A capture's operands address [vbase, vbase + size] of this frame's offset
   * space, a range no local and no other capture uses (gen_function); that
   * is what makes them chain-relative (tcc_ir_capture_lookup). */
  int32_t captured_vbase[32];
  int32_t captured_size[32];
  int32_t captured_space_end; /* one past the highest capture range */
  int32_t captured_count;            /* number of captured variables */
  int32_t loc;
  int32_t parent_loc; /* parent's loc value (for nested function offset validation) */
  /* Frontend stack objects as FRAME_OBJ_WORDS-word records (frame offset,
   * size incl. alignment padding, size, FRAME_OBJ_* flags | scope end, scope
   * start), for frame relayout (frame.c).  Filled while the body is parsed. */
  int32_t *frame_objs;
  int frame_obj_count, frame_obj_cap;
  /* Labels as IR_LABEL_WORDS-word records (Sym::jind, current instruction
   * index, anchor): the label stands in front of the instruction whose
   * orig_index is the anchor (first the one it was defined on), wherever a
   * pass moved it.  The index moves with every renumbering like a frame scope
   * end (frame.c), so `&&label` still finds its place once that instruction
   * itself is gone. */
#define IR_LABEL_WORDS 3
  int32_t *label_pos;
  int label_count, label_cap;
  /* Padding of the frame objects whose type the frontend knew, as (start,
   * bytes mapped, offset into frame_pad_bits) triples: bit b of the map =
   * byte b of the object is no member's (tcc_ir_frame_note_type). */
  int32_t *frame_pads;
  int frame_pad_count, frame_pad_cap;
  uint8_t *frame_pad_bits;
  int frame_pad_bits_len, frame_pad_bits_cap;
  /* By call id: the size of the struct written through parameter 0, the
   * struct-return buffer, or 0 (frame.c). */
  int32_t *sret_calls;
  /* By call id (-fyasc-facts, S4): what the callee's checked yasc parameter
   * types promise -- the passes may assume it (tcc_ir_yf_param). */
  struct IRYfCall *yf_calls;
  int yf_nb_calls;
  /* -fyasc-facts: this function's exclusive pointer parameters (vregs):
   * `T * restrict view` and `T * owned` -- nothing else in the caller reaches
   * what they point to while the call runs (O4) */
  int32_t yf_excl[8];
  int yf_nb_excl;
  /* -fyasc-facts: tcc_ir_opt_yf_promote ran on this function (it runs once) */
  int yf_promoted;
  /* By call id: the struct returned is empty (sret_calls says 1 for it). */
  uint8_t *sret_calls_empty;
  int sret_calls_size;
  /* Sorted, merged [start, end) extents of frame_objs for lookups, built for
   * frame_obj_count objects; frame_relaid once relayout moved them. */
  int32_t *frame_index;
  int frame_index_n, frame_index_objs;
  uint8_t frame_relaid;
  /* ssa:loop_unroll copied loads from a const table with the IV as index:
   * the constant-index loads it left fold (ssa:unroll_cascade). */
  uint8_t unrolled_table_loads;

  /* Optimization module data - opaque pointer to keep IR arch-independent */
  TCCFPMatCache *opt_fp_mat_cache;

  /* IROperand separate pools for cache efficiency */
  int64_t *pool_i64; /* 64-bit integer constants */
  int pool_i64_count;
  int pool_i64_capacity;

  uint64_t *pool_f64; /* 64-bit double bits */
  int pool_f64_count;
  int pool_f64_capacity;

  IRPoolSymref *pool_symref; /* symbol references */
  int pool_symref_count;
  int pool_symref_capacity;

  CType *pool_ctype; /* CType storage for struct/array operands */
  int pool_ctype_count;
  int pool_ctype_capacity;

  /* IROperand pool - stores compact 8-byte operands for all instruction operands.
   * Operand layout: dest (if present), src1 (if present), src2 (if present).
   * IRQuadCompact.operand_base indexes into this pool. */
  IROperand *iroperand_pool;
  int iroperand_pool_count;
  int iroperand_pool_capacity;

  /* Compact instruction array - parallel to instructions[] for now */
  IRQuadCompact *compact_instructions;
  int compact_instructions_size;
#ifdef CONFIG_TCC_BCHECK
  /* -b (frontend/gen/op/bcheck.c): pairs (result vreg, symref pool index of
   * the callee) of the __bound_ptr_add calls a dereference may turn into
   * __bound_ptr_indirN, and the addresses of the locals registered, dropped
   * at the function's exit */
  int *bc_patch;
  int bc_patch_n, bc_patch_cap;
  struct SValue *bc_locals;
  int bc_locals_n, bc_locals_cap;
#endif

  IRLiveInterval **active_set;

  IRLiveInterval *variables_live_intervals;
  int variables_live_intervals_size;
  int next_local_variable;

  IRLiveInterval *temporary_variables_live_intervals;
  int temporary_variables_live_intervals_size;
  int next_temporary_variable;
  /* ra_mark_rematerializable: (position, constant) pairs of the remat_kind 1
   * temps, sorted by position (tcc_ir_remat_imm); remat_imm_size pairs. */
  int32_t *remat_imm;
  int remat_imm_size;

  IRLiveInterval *parameters_live_intervals;
  int parameters_live_intervals_size;
  int next_parameter;

  int next_live_interval_index;
  int instructions_size;
  int next_instruction_index;
  int max_orig_index;           /* Highest orig_index ever assigned; updated in tcc_ir_put */
  int next_insn_is_jump_target; /* Pending flag: next tcc_ir_put must set is_jump_target=1 */

  /* Monotonic ID for binding FUNCPARAM* instructions to their owning FUNCCALL*.
   * Encoded in instruction operands for those ops.
   * 0 means "legacy/unknown" and falls back to nested-scan binding.
   */
  int next_call_id;
  /* -fcallgraph-info: each call's function type (tcc_stack_usage_sig) by call_id */
  unsigned short *su_call_sig;
  int su_nb_call_sig;
  /* -Wlifetime records (lifetime.c), freed once the function is checked */
  IRLtVar *lt_vars;
  int lt_nb_vars;
  IRLtEvent *lt_events;
  int lt_nb_events;
  IRLtCall *lt_calls; /* by call_id, lt_nb_calls entries */
  int lt_nb_calls;
  int *lt_ostores; /* [from, to) pairs: what a store into an owned pointer emitted */
  int32_t *lt_rlocals; /* (vreg, name) of the `T * restrict view` locals (S3) */
  int lt_nb_rlocals;
  /* (vreg, drop) of the checked `struct T * view` parameters: what their owned
     members hold when the function returns (the method self rules) */
  int32_t *lt_selfp;
  int lt_nb_selfp;
  /* the owned members of a `drop struct T * view` parameter's struct (vreg the
     parameter's): each must be released by the time the function returns */
  IRLtMember *lt_dmembers;
  int lt_nb_dmembers;
  /* (vreg, kind, callee) of an inlined callee's `T * view` parameter: binding
     the argument to it is that call's handing over (kind 1 view, 2 a const
     view, 3 drop) */
  int32_t *lt_iself;
  int lt_nb_iself;
  int32_t *lt_fnames; /* (frame offset, name) of local arrays and structs: names for messages */
  int lt_nb_fnames;
  int lt_nb_ostores;
  int *lt_rostores; /* [from, to, kind] triples: tcc_ir_lt_note_ro */
  int lt_nb_rostores;
  int32_t *lt_mtemps; /* temps holding move(p) handed to a regular-C parameter: p moves there */
  int lt_nb_mtemps;
  int *lt_fstores; /* [from, to, entry] triples: a store into a borrows(member) member */
  int lt_nb_fstores;
  int *lt_floads; /* [temp, entry] pairs: the temp holds a borrows(member) member's address */
  int lt_nb_floads;
  int *lt_fsrcs; /* [temp, entry] pairs: the temp holds the address of a member ENTRY borrows from */
  int lt_nb_fsrcs;
  int *lt_istores; /* [from, to, tok]: an initializer's store into a pointer member (tok; 0: an element) that
                      is not maybe -- the value must not be null */
  int lt_nb_istores;
  int *lt_astores; /* [from, to, tok, deref]: `a[i] = v` / `*p = v` (deref) into a pointer that is not maybe,
                      tok the name it starts from (0: none) -- the value must not be null */
  int lt_nb_astores;
  IRLtSlot *lt_slots;     /* owned items of local structs and arrays */
  int lt_nb_slots;
  IRLtSobj *lt_sobjs;     /* local structs whose values borrow */
  int lt_nb_sobjs;
  /* an inlined call's borrowing-struct result (-O1+): [idx, loc, n, var0..3]
     -- before instruction idx the result at frame offset loc borrows what
     the parameter variables var0.. were bound to (n -1: too many) */
  int32_t *lt_sres;
  int lt_nb_sres;
  /* an inlined call's view pointer result (-O1+): [var, n, var0..3] -- the
     result variable borrows what the parameter variables var0.. were bound
     to (n -1: too many), as the call's result would */
  int32_t *lt_pres;
  int lt_nb_pres;
  int32_t lt_ip_var[28]; /* the outermost inlined call's parameter variables by position, -1 */
  int32_t *lt_inames; /* (vreg, token) pairs: the int locals and parameters, names of an index */
  int lt_nb_inames;
  IRLtMember *lt_members; /* maybe members read or written through a pointer */
  /* maybe members of a global struct (`g.next`): the symbol, the member's
     offset from it (lt_gm_syms[i], lt_gmembers[i].offset; vreg unused) */
  IRLtMember *lt_gmembers;
  struct Sym **lt_gm_syms;
  int lt_nb_gmembers;
  /* local structs (`s.next`): frame offset, size, name, silent (inlined) */
  int *lt_fobjs; /* 5 ints each */
  int lt_nb_fobjs;
  /* [from, to) instruction ranges of inlined bodies (outermost only) */
  int *lt_inl;
  int lt_nb_inl;
  IRTagNote *tag_notes; /* yasc tagged union member accesses (tcc_ir_tag_check) */
  int nb_tag_notes;
  IRW1cNote *w1c_notes; /* yasc register stores with w1c bits (tcc_ir_w1c_check) */
  int nb_w1c_notes;
  int32_t *w1c_vcalls;  /* ... the calls handed a pointer to volatile memory */
  int nb_w1c_vcalls;
  int lt_nb_members;
  int lt_end_line; /* the function body's closing brace */
  uint8_t lt_on;   /* yasc and -Wlifetime: record and check this function */
  /* -Warray-bounds=2 (bounds.c): the subscripts to check, [from, to) ranges
     of inlined bodies (outermost only) */
  uint8_t ab_on;
  uint8_t ab_uaf; /* -Wuse-after-free: the same walk follows freed pointers */
  struct IRAbSite *ab_sites;
  int ab_nb_sites;
  int *ab_inl;
  int ab_nb_inl;
  long long *ab_said; /* (line, index) pairs the level-1 loop pattern reported */
  int ab_nb_said;
  struct IRAbObj *ab_objs; /* the frame's objects, for accesses through pointers */
  int ab_nb_objs;
  /* -Wtainted-format (taint.c): the calls' facts by call_id, a source among them */
  uint8_t taint_on, taint_sources;
  struct IRTaintCall *taint_calls;
  int taint_nb_calls;
  int *taint_fobjs; /* the frame's objects: (offset, size) pairs */
  int taint_nb_fobjs;
  struct IRTaintInl *taint_inl; /* the inline expansions (outermost), calls for the summaries */
  int taint_nb_inl;
  /* -Wyasc-c-api (capi_check.c): the calls' facts by call_id, a source among them */
  uint8_t capi_on, capi_sources;
  struct IRCapiCall *capi_calls;
  int capi_nb_calls;
  uint8_t lt_nonnull_calls; /* a call passes something to a parameter that may not be null */
  uint8_t lt_maybe_calls;   /* a call's result may be null (dereferenced without a name) */
  /* -Wlifetime: the temps a LOAD gave a pointer (a bit per temp position):
     the operand's btype says INT32 for an int and a pointer alike */
  uint32_t *lt_ptr_temps;
  int lt_ptr_temps_words;
  /* -W[maybe-]uninitialized: locals declared without a value, and where
   * (idx) each declaration makes its variable uninitialised again */
  IRLtVar *un_vars;
  int un_nb_vars;
  /* -Wdangling-pointer / -Wreturn-local-addr: the pointer locals and
   * parameters by vreg, for their names */
  IRLtVar *dp_names;
  int dp_nb_names;
  IRLtEvent *un_events;
  int un_nb_events;
  /* ... the VAR positions of inlined callees' `out` parameters (a bit each), noted
   * before the lifetime check frees its variables */
  uint32_t *un_outparam;
  int un_outparam_words;
  /* ... yasc's `= undefined` arrays and structs (un_agg_check) */
  IRUnAgg *un_aggs;
  int un_nb_aggs;
  /* The VARs the last SRA run on this function created, [lo, hi); -1 when it
   * made none or did not run.  ssa_var_promotable relaxes its rules for them. */
  int sra_var_lo, sra_var_hi;

  /* Current instruction index during code generation - used for scratch register allocation */
  int codegen_instruction_idx;

  /* Outgoing call argument area reserved in the function frame (FP-relative).
   * If non-zero, stack args are stored at [FP + call_outgoing_base + stack_off].
   */
  int call_outgoing_base; /* frame offset (typically negative) */
  int call_outgoing_size; /* bytes reserved (may include alignment padding) */
  /* Calls whose stack arguments do not fit the reserved area: per call id, the
   * bytes SP drops below it around that call alone (a `sub sp` ... `add sp`
   * window, see ir/codegen.c).  NULL when no call needs one. */
  int32_t *call_dyn_extra;
  int call_dyn_extra_size;

  /* Nested-call register save area: reserved in the frame for saving R0-R3
   * (and R9/R12 for alignment) across nested function calls without PUSH/POP.
   * Sits above the outgoing area in the frame layout. */
  int call_nested_save_base; /* frame offset (typically negative) */
  int call_nested_save_size; /* bytes reserved (0 if no nested calls possible) */

  /* Scratch register save area: reserved when FP is omitted so that
   * get_scratch_reg_with_save() can use STR/LDR instead of PUSH/POP.
   * This prevents SP movement that would break SP-relative addressing. */
  int scratch_save_base; /* frame offset (typically negative) */
  int scratch_save_size; /* bytes reserved (0 when FP is used) */

  uint32_t *ignored_vregs;
  int ignored_vregs_size;

  SpillCache spill_cache; // Cache for tracking register-stack mappings during codegen
  TCCStackLayout stack_layout;

#ifdef CONFIG_TCC_ASM
  /* Inline asm blocks recorded during IR building, lowered during codegen. */
  TCCIRInlineAsm *inline_asms;
  int inline_asm_count;
  int inline_asm_capacity;
#endif

  /* Mapping from IR instruction index to generated machine code offset (section-relative).
   * Size is (next_instruction_index + 1) to include the epilogue mapping.
   * This is populated during tcc_ir_codegen_generate() and is used after codegen
   * for features like GCC's labels-as-values (&&label). */
  uint32_t *ir_to_code_mapping;
  int ir_to_code_mapping_size;

  /* Mapping from ORIGINAL IR instruction index (pre-DCE/compaction) to generated
   * machine code offset. Label positions (s->jind) are recorded before DCE, so
   * this mapping is the correct one to use for &&label materialization.
   */
  uint32_t *orig_ir_to_code_mapping;
  int orig_ir_to_code_mapping_size;

  /* Mirror of tccgen's func_has_label_addr for the current function: set when the
   * body takes a label address (GCC labels-as-values, `&&label`).  Kept on the IR
   * state so the IR layer (regalloc) can consult it without referencing a tccgen
   * global (which the standalone unit-test link does not provide). */
  int func_has_label_addr;
  /* SSA promotion and transformations requiring edge-specific copies still exclude asm goto. */
  int func_has_asm_goto;
  /* The return class ra:known_ext proved for this body (FuncAttr.func_ret_zext),
   * published by gen_function once the body is final. */
  int ret_zext_class;

  /* Set the first time IR generation builds a VOLATILE lvalue operand for this
   * function.  Clear means the body performs no volatile access at all, which
   * lets every CSE/DSE/forwarding pass skip its per-operand volatility check —
   * and, more importantly, keeps them from being blinded by an operand a pass
   * synthesized without the (proven-non-volatile) mark.  See
   * tcc_ir_access_is_volatile below and IROP_AUX_NONVOLATILE. */
  int func_has_volatile_access;
  int func_has_volatile_local; /* a local or parameter declared volatile */

  /* ir_opt_fn_features (opt_utils.h): the opcode-presence bits of this body,
   * valid while fn_features_gen == opt_generation != 0.  The flat engine sets
   * opt_generation to its change counter around each plain pass call and
   * clears it after, so passes it runs share one scan per generation and
   * every other caller scans afresh. */
  uint32_t fn_features, fn_features_gen, opt_generation;
  /* ir_opt_frame_ptr (opt_alias.h): the points-to facts, same validity rule. */
  void *frame_ptr;
  uint32_t frame_ptr_gen;

  LSLiveIntervalState ls;

  /* Extra scratch allocation flags to apply during materialization for the current IR instruction. */
  unsigned codegen_materialize_scratch_flags;

  /* Set between CMP and JUMPIF emission so the backend uses flag-preserving encodings. */
  int codegen_flags_live;

  /* Switch tables for jump table generation */
  TCCIRSwitchTable *switch_tables;
  int num_switch_tables;
  int switch_tables_capacity;

  /* The switches the frontend laid out as `JMP dispatch; bodies; dispatch`,
   * as (jump, dispatch, end) index triples in the order they closed
   * (opt/flat/cfg/switch_head.c). */
  int *switch_heads;
  int switch_heads_n, switch_heads_cap;

  /* Switch value tables for SWITCH_LOAD (constant-table dispatch). */
  TCCIRSwitchValueTable *switch_value_tables;
  int num_switch_value_tables;
  int switch_value_tables_capacity;

  /* Barrel shift annotations: populated just before codegen, freed after.
   * barrel_shifts[i] encodes an optional barrel shift on src2 of instruction i:
   * 0 = none, else (type<<5)|amount. type: 1=SHL, 2=SHR, 3=SAR, 4=ROR. */
  uint8_t *barrel_shifts;
  /* Element count at allocation; reads bound by this, not max_orig_index. See docs/side_table_orig_index_bounds.md */
  int barrel_shifts_len;

  /* Dead-half annotations for 64-bit shift ops, keyed by orig_index.
   * Populated just before codegen, freed after.  bit0 = the result's low
   * word is dead (no consumer reads it); bit1 = the result's high word is
   * dead.  Lets thumb_emit_shift64_mop skip the dead half-write in the
   * 64-bit bitfield-extract idiom (SHL #a; SHR #b, b>=32). */
  uint8_t *shift64_dead_half;
  int shift64_dead_half_len;

  /* Known-zero / dead 64-bit halves, keyed by orig_index.  Populated just
   * before codegen by tcc_ir_opt_zero_half64, freed after.  bits 0-3 say which
   * halves of src1/src2 are provably the constant zero; bits 4-5 say which
   * halves of the destination no consumer reads and therefore need not be
   * written.  See source/opt/flat/fusion/zero_half64.c. */
  uint8_t *zero_half64;
  int zero_half64_len;

  /* BFI insert parameters, keyed by orig_index.  Populated by
   * tcc_ir_opt_bitfield_insert_to_bfi just before codegen, freed after.
   * Entry = lsb (bits 0-7) | (width << 8); width >= 1 so a real BFI entry is
   * never 0.  Consumed by tcc_gen_machine_bfi_mop. */
  uint16_t *bfi_params;
  int bfi_params_len;

  /* Codegen temporaries owned by tcc_ir_codegen_generate while it is running.
   * They are normally freed before return; tcc_ir_free also releases them when
   * a compile error longjmps out of codegen. */
  int *codegen_return_jump_addrs;
  int *codegen_dry_insn_scratch;
  uint16_t *codegen_dry_insn_saves;
  uint32_t *codegen_dry_insn_save_info; /* per instruction: see IR_SAVE_INFO_* (codegen.c) */
  void *codegen_mop_cache;
  uint32_t *codegen_cbz_dry_mapping;
  /* Code address just past the last body instruction as laid out by the
   * rehearsal pass — i.e. where the epilogue begins.  Return jumps target the
   * epilogue, which is not an IR index, so this is what lets them be sized.
   * 0 when no rehearsal ran. */
  uint32_t codegen_rehearsal_end;
  /* The rehearsal's running literal-pool entry total there. */
  uint32_t codegen_rehearsal_end_pool_entries;
  /* Per IR instruction, 1 when the rehearsal laid it out as zero bytes (after
   * its alignment pad): a coalesced phi copy, a NOP.  A JUMP or a return whose
   * span holds only such ops is dropped by the real pass
   * (tcc_ir_codegen_jump_elidable).  NULL when no rehearsal ran. */
  uint8_t *codegen_rehearsal_empty;
  /* Running literal-pool entry total per IR instruction, recorded by the
   * rehearsal pass; bounds the pool pressure over a forward branch's range so
   * can_narrow_forward_branch can prove no flush lands inside it. */
  uint16_t *codegen_dry_pool_entries;
  uint8_t *codegen_branch_target_reset;
  int *codegen_temp_uses; /* ir_codegen_temp_uses's, for tcc_ir_free after an error */

  /* Per-vreg def and use counts, open only while a pass that queries them per
   * candidate runs (tcc_ir_vreg_index_open, opt/util/vreg_def_use.c); NULL
   * otherwise.  Answers tcc_ir_vreg_has_single_use/_single_def/_multi_def and
   * tcc_ir_find_defining_instruction without a whole-function scan. */
  struct IRVregIndex *vreg_index;
} TCCIRState;

/* The constant temp VREG is rematerialized as (remat_kind 1). */
static inline int32_t tcc_ir_remat_imm(const TCCIRState *ir, int vreg)
{
  const int p = TCCIR_DECODE_VREG_POSITION(vreg);
  int lo = 0, hi = ir->remat_imm_size - 1;
  while (lo <= hi)
  {
    const int mid = (lo + hi) >> 1, q = ir->remat_imm[2 * mid];
    if (q == p)
      return ir->remat_imm[2 * mid + 1];
    if (q < p)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return 0;
}

/* Volatility queries for the optimizer, in the two shapes memory ops come in:
 * `op` is the deref (lvalue) operand of a LOAD/STORE, or the BASE operand of a
 * LOAD_INDEXED/STORE_INDEXED (the fusion passes transfer the mark onto it, see
 * irop_carry_access_marks).  A pass that removes, reuses or reorders a memory
 * access must ask THIS, not the accessed Sym's type: a Sym cannot express
 * `volatile int a[4]` (the qualifier is on the element type), a volatile member
 * of a plain struct, a `(volatile T *)` cast, or a bare `T***DEREF***`.
 *
 * The function-level flag comes first so that a body with no volatile access at
 * all — nearly all of them — answers "not volatile" for every operand, including
 * the ones passes synthesize without a mark.  Only inside a function that really
 * does touch volatile memory does the conservative per-operand default bite. */
static inline int tcc_ir_access_is_volatile(const TCCIRState *ir, IROperand op)
{
  return ir->func_has_volatile_access && irop_access_is_volatile(op);
}

/* IR invariant scanners, called after every pass of every function from the
 * pipeline driver and from ~25 hand-placed probes.  SCAN_OVERLAP is O(n^2) over
 * the instruction array, so both the calls and the bodies are compiled out when
 * the debug knobs are off (source/opt/engine/pipeline_run.c holds the bodies).
 * Declared here rather than per-caller so the release no-op reaches every site. */
#if CONFIG_TCC_DEBUG_ENV
void dbg_scan_overlap(TCCIRState *ir, const char *pass);
void dbg_scan_imm_dest(TCCIRState *ir, const char *pass);
#else
#define dbg_scan_overlap(ir, pass) ((void)0)
#define dbg_scan_imm_dest(ir, pass) ((void)0)
#endif

/* TCC_DISABLE_PASS bisection gate, queried once per pass per function — several
 * thousand calls per compile.  With the debug knobs off there is nothing to look
 * up, so it folds to an inline `return 0` and the call goes with it.  Declared
 * here, not in opt_utils.h, because callers in ir/ never include that header. */
#if CONFIG_TCC_DEBUG_ENV
int tcc_ir_opt_pass_disabled(const char *name);
#else
static inline int tcc_ir_opt_pass_disabled(const char *name)
{
  (void)name;
  return 0;
}
#endif

/* analysis/frame_ptr.c: drops the cached points-to facts (opt_alias.h). */
void ir_opt_frame_ptr_free(TCCIRState *ir);

TCCIRState *tcc_ir_allocate_block();

/* If the value is an lvalue (memory reference), emit an IR load so the
 * SValue becomes a plain value suitable for arithmetic/indirect calls. */

int tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest);

#ifdef CONFIG_TCC_ASM
int tcc_ir_add_inline_asm(TCCIRState *ir, const char *asm_str, int asm_len, int must_subst, ASMOperand *operands,
                          int nb_operands, int nb_outputs, int nb_labels, const uint8_t *clobber_regs);
void tcc_ir_put_inline_asm(TCCIRState *ir, int inline_asm_id);
#endif
int tcc_ir_asm_goto_target(TCCIRState *ir, const IRQuadCompact *q, int label);
void tcc_ir_asm_label_note(TCCIRState *ir, int sym, int jind);
void tcc_ir_asm_mark_targets(TCCIRState *ir);

int tcc_ir_get_vreg_temp(TCCIRState *ir);
int tcc_ir_get_vreg_var(TCCIRState *ir);

/* -Wlifetime: yasc ownership flow on the raw IR (lifetime.c) */
int tcc_ir_lt_active(void);
void tcc_ir_lt_begin(TCCIRState *ir);
/* -Warray-bounds=2: value ranges of subscripts on the raw IR (bounds.c) */
int tcc_ir_ab_active(void);
void tcc_ir_ab_begin(TCCIRState *ir);
int tcc_ir_ab_site(TCCIRState *ir, int32_t vreg, int is_unsigned, int n, int line, const char *type);
void tcc_ir_ab_site_mid(TCCIRState *ir, int site);
void tcc_ir_ab_site_end(TCCIRState *ir, int site, int one_past_ok);
void tcc_ir_ab_inline_mark(TCCIRState *ir, int begin);
void tcc_ir_ab_said(TCCIRState *ir, int line, long long idx);
void tcc_ir_ab_add_local(TCCIRState *ir, struct Sym *sym);
void tcc_ir_ab_check(TCCIRState *ir);
void tcc_ir_ab_free(TCCIRState *ir);
/* -Wyasc-c-api (capi_check.c) */
void tcc_ir_capi_begin(TCCIRState *ir);
void tcc_ir_capi_note_call(TCCIRState *ir, int call_id, int callee, struct Sym *ftype);
void tcc_ir_capi_check(TCCIRState *ir, struct Sym *func);
void tcc_ir_capi_free(TCCIRState *ir);
void tcc_ir_capi_copy_call(TCCIRState *ir, int from, int to);
/* -Wtainted-format (taint.c) */
int tcc_ir_taint_active(void);
void tcc_ir_taint_begin(TCCIRState *ir);
void tcc_ir_taint_note_call(TCCIRState *ir, int call_id, struct Sym *callee, struct Sym *ftype);
void tcc_ir_taint_check(TCCIRState *ir, struct Sym *func);
void tcc_ir_taint_free(TCCIRState *ir);
void tcc_ir_taint_add_local(TCCIRState *ir, struct Sym *sym);
void tcc_ir_taint_inline_param(TCCIRState *ir, int index, struct Sym *psym);
void tcc_ir_taint_inline_mark(TCCIRState *ir, int begin, struct Sym *callee, int ret_vreg);
void tcc_ir_taint_unit_end(void);
void tcc_ir_taint_unit_free(void);
void tcc_ir_lt_add_var(TCCIRState *ir, int32_t vreg, struct Sym *sym);
void tcc_ir_lt_silence(TCCIRState *ir, int32_t vreg);
void tcc_ir_lt_implicit_view(TCCIRState *ir, int32_t vreg);
void tcc_ir_lt_scope_end(TCCIRState *ir, struct Sym *top, struct Sym *bottom);
void tcc_ir_lt_return(TCCIRState *ir, int line);
void tcc_ir_lt_inline_param(TCCIRState *ir, int32_t vreg, struct Sym *callee, int index);
void tcc_ir_lt_inline_result(TCCIRState *ir, int32_t vreg, struct Sym *ftype);
void tcc_ir_lt_inline_sresult(TCCIRState *ir, int loc, struct Sym *ftype, struct Sym *callee);
void tcc_ir_lt_note_call(TCCIRState *ir, int call_id, struct Sym *ftype, struct Sym *callee);
void tcc_ir_lt_note_implicit(TCCIRState *ir, int call_id);
void tcc_ir_lt_copy_call(TCCIRState *ir, int from, int to);
void tcc_ir_lt_note_owned_store(TCCIRState *ir, int from, int to);
/* yasc: instructions [from, to) store a read-only value into a plain local
   (LT_RO_LOCAL: it holds one now), or store into memory whose pointer is to
   const (LT_RO_CONSTDEST: a read-only value may go there) */
#define LT_RO_LOCAL 1
#define LT_RO_CONSTDEST 2
void tcc_ir_lt_note_ro(TCCIRState *ir, int from, int to, int kind);
void tcc_ir_lt_note_moved_temp(TCCIRState *ir, int32_t vreg);
void tcc_ir_lt_note_member(TCCIRState *ir, int32_t vreg, int offset, int tok, int owned, int esize);
void tcc_ir_note_tagged(TCCIRState *ir, int kind, int32_t vr, int32_t off, struct Sym *sym, int tu, int m, int line);
void tcc_ir_tag_check(TCCIRState *ir);
int tcc_ir_call_writes_global(int gkeep, struct Sym *sym);
void tcc_ir_note_w1c(TCCIRState *ir, const IRW1cNote *note);
void tcc_ir_w1c_check(TCCIRState *ir);
void tcc_ir_w1c_free(TCCIRState *ir);
void tcc_ir_note_vcall(TCCIRState *ir, int call_id);
void tcc_ir_lt_note_gmember(TCCIRState *ir, struct Sym *sym, int offset, int tok, int owned, int foff, int esize);
void tcc_ir_lt_inline_mark(TCCIRState *ir, int begin);
void tcc_ir_lifetime_check(TCCIRState *ir, struct Sym *func);
void tcc_ir_lt_free(TCCIRState *ir);
int tcc_ir_un_wanted(void);
void tcc_ir_un_add_var(TCCIRState *ir, int32_t vreg, struct Sym *sym);
void tcc_ir_un_add_agg(TCCIRState *ir, int off, int size, struct Sym *sym);
void tcc_ir_flow_checks(TCCIRState *ir, struct Sym *func);
void tcc_ir_null_deref_check(TCCIRState *ir);
int tcc_ir_dp_wanted(void);
void tcc_ir_view_escape_check(TCCIRState *ir, struct Sym *func);
void tcc_ir_lt_note_pointer(TCCIRState *ir, int vreg);
void tcc_ir_dealloc_check(TCCIRState *ir);
void tcc_ir_restrict_check(TCCIRState *ir);
/* -fyasc-facts (S4): per call, by declared parameter (bit i) */
typedef struct IRYfCall
{
  uint32_t noescape; /* a `view` parameter not named by the result's borrows(...): the callee keeps no copy */
  uint32_t readonly; /* of them, `const T * view`: the callee does not write through it */
  uint32_t drop;     /* `drop T * owned` parameters: the callee destroys the object, keeps no pointer */
  uint8_t fresh;     /* the result is `owned`: new memory nothing older points into (O2) */
  uint8_t noted;
} IRYfCall;
void tcc_ir_yf_note_call(TCCIRState *ir, int call_id, struct Sym *ftype);
/* the parameter at IR index PIDX of call CALL_ID (a struct-return buffer
   counted): 1 when it does not escape, *readonly set */
int tcc_ir_yf_param(const TCCIRState *ir, int call_id, int pidx, int *readonly);
int tcc_ir_yf_param_drop(const TCCIRState *ir, int call_id, int pidx);
int tcc_ir_yf_result_fresh(const TCCIRState *ir, int call_id);
void tcc_ir_lt_note_slot(TCCIRState *ir, int addr, struct CType *type, int tok, int obj_dealloc);
void tcc_ir_slot_check(TCCIRState *ir, struct Sym *func);
void tcc_ir_lt_note_field_store(TCCIRState *ir, int from, int to, int entry);
void tcc_ir_lt_note_field_load(TCCIRState *ir, int idx, int entry);
void tcc_ir_lt_note_field_src(TCCIRState *ir, int vreg, int entry);
void tcc_ir_lt_note_init_store(TCCIRState *ir, int from, int to, int tok);
void tcc_ir_lt_note_assign_store(TCCIRState *ir, int from, int to, int tok, int deref);
void tcc_ir_dp_add_name(TCCIRState *ir, int32_t vreg, struct Sym *sym);
int tcc_ir_flow_checks_wanted(void);
int tcc_ir_get_vreg_param(TCCIRState *ir);
/* Allocate static chain vreg for nested functions (live-in at R10) */
int tcc_ir_get_vreg_static_chain(TCCIRState *ir);

void tcc_ir_set_float_type(TCCIRState *ir, int vreg, int is_float, int is_double);
void tcc_ir_set_llong_type(TCCIRState *ir, int vreg);
void tcc_ir_set_original_offset(TCCIRState *ir, int vreg, int offset);
/* frame.c */
/* `(loc - size) & mask`, recording the object in the current function's IR
 * for tcc_ir_frame_relayout. */
int tcc_ir_frame_alloc(int loc, int size, int mask);
void tcc_ir_frame_scope_end(struct TCCIRState *ir, int first_obj, int insn, int keep);
void tcc_ir_frame_scope_block(struct TCCIRState *ir, int first_obj, int start, int end);
/* Keep those scope ends (instruction indices) in step with passes that move
 * instructions: an insert before `before_idx`, a renumbering (map[old] = new
 * or -1), or code duplicated/reordered within lo..hi. */
void tcc_ir_frame_scope_insert(struct TCCIRState *ir, int before_idx);
void tcc_ir_frame_scope_remap(struct TCCIRState *ir, const int *map, int old_n, int new_n);
void tcc_ir_frame_scope_widen(struct TCCIRState *ir, int lo, int hi);
void tcc_ir_label_note(struct TCCIRState *ir, int jind);
int tcc_ir_label_insn(struct TCCIRState *ir, int jind);
void tcc_ir_keep_label_ahead(struct TCCIRState *ir, int pos);
void tcc_ir_label_sync(struct TCCIRState *ir);
/* The same for the temporaries that carry a struct into or out of a call: its
 * address reaching the call does not let the callee keep it.  An argument's
 * copy is never visible to the program; a call's struct result is, through
 * an array member (`f().arr`), until the end of the full expression. */
int tcc_ir_frame_alloc_arg_copy(int loc, int size, int mask);
int tcc_ir_frame_alloc_ret_temp(int loc, int size, int mask);
/* Parameter 0 of call `call_id` is the struct-return buffer. */
void tcc_ir_frame_note_sret_call(int call_id, int size);
/* The frontend object containing frame offset `off`, as [*lo, *hi), while the
 * frame still has the frontend's layout.  0 when unknown. */
int tcc_ir_frame_object_at(TCCIRState *ir, int off, int *lo, int *hi);
/* Recorded size of the frame object starting exactly at `start`, or -1
 * (before frame relayout only). */
int tcc_ir_frame_object_size_at(TCCIRState *ir, int start);
/* The object just allocated at `off` has C type `type`: note the bytes of it
 * no member covers. */
void tcc_ir_frame_note_type(int off, CType *type);
/* Bytes of [lo, hi) that are padding in every object recorded there, bit b =
 * byte lo + b; 0 when any object overlapping it has no noted type or starts
 * elsewhere (before frame relayout only). */
uint64_t tcc_ir_frame_padding(TCCIRState *ir, int lo, int hi);
/* The same for an object of any size: pad[b] = 1 for byte lo + b; returns
 * whether any byte is padding (pad all 0 when unknown). */
int tcc_ir_frame_padding_map(TCCIRState *ir, int lo, int hi, uint8_t *pad);
int tcc_ir_frame_relayout(TCCIRState *ir, int *ploc);
/* The k-th concrete frame offset an inline asm operand names outside the IR
 * (the SValues the front end saved with the statement): 1 and *off, or 0 when
 * there are no more. */
int tcc_ir_asm_frame_ref(TCCIRState *ir, int k, int *off);
int tcc_ir_frame_range_referenced(TCCIRState *ir, int off, int size);
/* The function calls setjmp, vfork or another function that returns twice, so
 * a frame slot live only on one return may still be read on the other. */
int tcc_ir_calls_returns_twice(TCCIRState *ir);
int tcc_ir_cross_jump(TCCIRState *ir);
int tcc_ir_cross_jump_alloc(TCCIRState *ir);
int tcc_ir_region_merge(TCCIRState *ir);
int tcc_ir_branch_tidy(TCCIRState *ir);
int tcc_ir_drop_jumpif_to_next(TCCIRState *ir);
int tcc_ir_self_store(TCCIRState *ir);
int tcc_ir_dead_def(TCCIRState *ir);
struct IRSSAState;
int tcc_ir_known_ext(TCCIRState *ir, struct IRSSAState *ssa);
uint32_t tcc_ir_ret_zext_mask(int ret_class);
/* Writes into frame bytes nothing reads go, frame objects split into the
 * pieces still referenced (frame_dfe.c).  *inserted: instructions it added
 * (the CFG must be rebuilt). */
int tcc_ir_frame_dead_bytes(TCCIRState *ir, int *inserted);
int tcc_ir_get_reg_type(TCCIRState *ir, int vreg);

void tcc_ir_register_allocation_params(TCCIRState *ir);
/* For parameters that arrive on the caller stack (beyond r0-r3 per AAPCS),
 * do not allocate separate local spill slots. They already have a stable
 * incoming stack home for the duration of the call. */
void tcc_ir_mark_return_value_incoming_regs(TCCIRState *ir);
void tcc_ir_avoid_spilling_stack_passed_params(TCCIRState *ir);
int tcc_ir_param_is_captured_by_nested(int vreg);
/* This function's frame is reached by code outside its IR (it has, or is, a
 * nested function), so a call may read or write any local. */
int tcc_ir_frame_shared_with_nested(const TCCIRState *ir);
/* Frame offset of a variadic function's r0 save (r0-r3 at +0..+12): -16, or
 * -20 with a static chain, whose save slot is frame top - 4. */
static inline int tcc_ir_variadic_reg_save_base(const TCCIRState *ir)
{
  return ir && ir->has_static_chain ? -20 : -16;
}
void tcc_ir_mark_nested_captured_params(TCCIRState *ir);
void tcc_ir_build_stack_layout(TCCIRState *ir);
/* Caller-saved registers to save around the call at call_idx (ra:caller_save) */
uint32_t tcc_ir_caller_save_mask_at(TCCIRState *ir, int call_idx);
const TCCStackSlot *tcc_ir_stack_slot_by_vreg(const TCCIRState *ir, int vreg);
const TCCStackSlot *tcc_ir_stack_slot_by_offset(const TCCIRState *ir, int frame_offset);
void tcc_ir_assign_physical_register(TCCIRState *ir, int vreg, int offset, int r0, int r1);
const char *tcc_ir_get_op_name(TccIrOp op);
void tcc_ir_show(TCCIRState *ir);
void tcc_ir_dump_set_show_physical_regs(int show);
/* -dump-ir-passes= helpers (shared by the legacy optimize loop in tccgen.c and
 * the SSA optimizer driver in ir/opt/ssa_opt.c). */
int tcc_ir_dump_passes_match(TCCState *s, const char *pass_name);
#ifdef CONFIG_TCC_DEBUG
void tcc_ir_dump_after_pass(TCCIRState *ir, const char *pass_name);
#else
#define tcc_ir_dump_after_pass(ir, pass_name) ((void)(ir))
#endif
/* The same dump, printed the way SSA is written down: one section per basic
 * block, the phi nodes at the head of it, then that block's instructions.  A
 * phi is NOT in the instruction array -- it lives in IRSSAState.block_phis --
 * so the flat dump above cannot show one at all, and its listing after
 * `ssa_rename` reads names nothing in it defines.  Selected by the same
 * -dump-ir-passes=<name> list; the pipeline offers `ssa_phi` (straight after
 * renaming) and `ssa_phi_opt` (after the SSA pass pipeline). */
struct IRSSAState;
void tcc_ir_dump_ssa_after_pass(TCCIRState *ir, struct IRSSAState *ssa, const char *pass_name);
void tcc_ir_set_addrtaken(TCCIRState *ir, int vreg);

IRLiveInterval *tcc_ir_get_live_interval(TCCIRState *ir, int vreg);
IRLiveInterval *tcc_ir_try_get_live_interval(TCCIRState *ir, int vreg);
void tcc_ir_backpatch(TCCIRState *ir, int t, int target_address);
void tcc_ir_backpatch_to_here(TCCIRState *ir, int t);
void tcc_ir_backpatch_first(TCCIRState *ir, int t, int target_address);
int tcc_ir_gjmp_append(TCCIRState *ir, int n, int t);
void tcc_ir_print_vreg(int vreg);
void print_iroperand_short(TCCIRState *ir, IROperand op);
void tcc_print_quadruple_irop(TCCIRState *ir, IRQuadCompact *q, int pc);

/* Machine-independent spill helpers (defined in tccir.c) */
int tcc_ir_is_spilled(SValue *sv);
int tcc_ir_is_64bit(int t);

/* Machine-dependent spill handling (defined in machine-specific code, e.g., arm-thumb-gen.c) */

/* Spill cache management for avoiding redundant loads */
void tcc_ir_spill_cache_clear(SpillCache *cache);
void tcc_ir_spill_cache_record(SpillCache *cache, int reg, int offset);
int tcc_ir_spill_cache_lookup(SpillCache *cache, int offset);
void tcc_ir_spill_cache_invalidate_reg(SpillCache *cache, int reg);
void tcc_ir_spill_cache_invalidate_offset(SpillCache *cache, int offset);

/* SValue pool accessor functions for compact IR storage.
 * Operand layout in pool: dest (if present), src1 (if present), src2 (if present).
 * Returns NULL if the operand is not used by this operation. */

int ir_op_slot_count(TccIrOp op);

/* ============================================================================
 * IROperand pool accessor functions - compact 8-byte operand access
 * ============================================================================
 * Operand layout: dest (if present), src1 (if present), src2 (if present).
 * Returns IROP_NONE if the operand is not used by this operation.
 */

/* dest/src1/src2 decode -- called from every pass; ~7% of a -O0 compile
 * between them.  Inlined for the host cross only (see TCC_IROP_INLINE_ACCESSORS
 * in tccir_operand.h); bodies must stay identical to the out-of-line copies in
 * tccir_operand.c, which are guarded by the same macro. */
#ifdef TCC_IROP_INLINE_ACCESSORS
static inline IROperand tcc_ir_op_get_dest(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!irop_config[q->op].has_dest)
    return IROP_NONE;
  if (q->operand_base >= (uint32_t)ir->iroperand_pool_count)
    return IROP_NONE;
  return ir->iroperand_pool[q->operand_base];
}

static inline IROperand tcc_ir_op_get_src1(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!irop_config[q->op].has_src1)
    return IROP_NONE;
  int off = irop_config[q->op].has_dest;
  if (q->operand_base + off >= (uint32_t)ir->iroperand_pool_count)
    return IROP_NONE;
  return ir->iroperand_pool[q->operand_base + off];
}

static inline IROperand tcc_ir_op_get_src2(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!irop_config[q->op].has_src2)
    return IROP_NONE;
  int off = irop_config[q->op].has_dest + irop_config[q->op].has_src1;
  if (q->operand_base + off >= (uint32_t)ir->iroperand_pool_count)
    return IROP_NONE;
  return ir->iroperand_pool[q->operand_base + off];
}
#else
/* An IROperand is 8 bytes, which AAPCS returns through memory: a stack slot in each
 * caller, a store here and a load there, 443 million times in a self-compile at -O2.
 * A reference needs none of it, and a caller that reads one field loads one word.
 * The expression is still a copy wherever one is taken. */
const IROperand *tcc_ir_op_dest_ref(const TCCIRState *ir, const IRQuadCompact *q);
#define tcc_ir_op_get_dest(ir, q) (*tcc_ir_op_dest_ref((ir), (q)))
#endif

/* A scan of a whole function for one vreg can compare words instead of copying each
 * operand out: irop_get_vreg(op) == vr, for a real vreg vr, means exactly
 * IROP_VREG_BITS(op) == vr (position in bits 0-16, type in bits 28-31). */
#define IROP_VREG_BITS(op) ((uint32_t)(op).vr & (((uint32_t)7 << TCCIR_VREG_TYPE_SHIFT) | TCCIR_VREG_POSITION_MASK))

/* How many of q's pool slots hold its dest, src1, src2 and MLA's accumulator. */
static inline int tcc_ir_quad_operand_words(const TCCIRState *ir, const IRQuadCompact *q)
{
  const IRRegistersConfig cfg = irop_config[q->op];
  int n = q->op == TCCIR_OP_MLA ? 4 : cfg.has_dest + cfg.has_src1 + cfg.has_src2;
  if ((int)q->operand_base + n > ir->iroperand_pool_count)
    n = ir->iroperand_pool_count - (int)q->operand_base;
  return n;
}

IROperand tcc_ir_get_dest(const TCCIRState *ir, int index);

/* Side tables keyed by orig_index; a 0 result means "no annotation" (including
 * for instructions inserted after the table was populated). See docs/side_table_orig_index_bounds.md */
uint8_t tcc_ir_barrel_shift_at(const TCCIRState *ir, const IRQuadCompact *q);

uint8_t tcc_ir_shift64_dead_half_at(const TCCIRState *ir, const IRQuadCompact *q);

uint8_t tcc_ir_zero_half64_at(const TCCIRState *ir, const IRQuadCompact *q);

/* UBFX/SBFX parameter word: lsb in bits 0-4, width in bits 5-9.  Bit 12 says
 * the field lives in the HIGH word of a 64-bit src1 -- `(v64 >> 52) & 0x7FF`
 * and friends, where the lsb stays 0..31 so every reader of the width and the
 * value range keeps working unchanged.  See
 * source/opt/flat/fusion/shift64_extract_ubfx.c. */
#define UBFX_HI_HALF 0x1000

uint16_t tcc_ir_bfi_params_at(const TCCIRState *ir, const IRQuadCompact *q);

#ifndef TCC_IROP_INLINE_ACCESSORS
const IROperand *tcc_ir_op_src1_ref(const TCCIRState *ir, const IRQuadCompact *q);
#define tcc_ir_op_get_src1(ir, q) (*tcc_ir_op_src1_ref((ir), (q)))
#endif

IROperand tcc_ir_get_src1(const TCCIRState *ir, int index);

#ifndef TCC_IROP_INLINE_ACCESSORS
const IROperand *tcc_ir_op_src2_ref(const TCCIRState *ir, const IRQuadCompact *q);
#define tcc_ir_op_get_src2(ir, q) (*tcc_ir_op_src2_ref((ir), (q)))
#endif

IROperand tcc_ir_get_src2(const TCCIRState *ir, int index);

/* Does this slot hold vreg V?  For scans over a function's quads, which ask nothing else:
 * no call, and no decoding, since a real vreg is the operand's type and position bits
 * as they stand.  V must be real: an operand with no vreg decodes to -1 or lower, and a
 * value with no type bits would match the words of a constant.  What irop_get_vreg
 * returned, once tested >= 0, is real; a scan that cannot say so uses IR_SLOT_HOLDS. */
#define IROP_VREG_WORD_MASK (((uint32_t)7 << TCCIR_VREG_TYPE_SHIFT) | (uint32_t)TCCIR_VREG_POSITION_MASK)

static inline int tcc_ir_vreg_is_real(int32_t v)
{
  const uint32_t type = (uint32_t)v >> TCCIR_VREG_TYPE_SHIFT;
  return type - 1u < 3u && ((uint32_t)v & ~IROP_VREG_WORD_MASK) == 0; /* VAR, TEMP, PARAM */
}

/* irop_config[op] as the byte it is: has_dest, has_src1, has_src2 from bit 0. */
#define IROP_CFG_BYTE(op) (((const uint8_t *)irop_config)[op])

#define IROP_SLOT_HOLDS(ir, at, v) (((uint32_t)(ir)->iroperand_pool[at].vr & IROP_VREG_WORD_MASK) == (uint32_t)(v))

static inline int tcc_ir_op_dest_is_vreg(const TCCIRState *ir, const IRQuadCompact *q, int32_t v)
{
  const uint32_t at = q->operand_base;
  return (IROP_CFG_BYTE(q->op) & 1) && IROP_SLOT_HOLDS(ir, at, v);
}

static inline int tcc_ir_op_src1_is_vreg(const TCCIRState *ir, const IRQuadCompact *q, int32_t v)
{
  const unsigned cfg = IROP_CFG_BYTE(q->op);
  const uint32_t at = q->operand_base + (cfg & 1);
  return (cfg & 2) && IROP_SLOT_HOLDS(ir, at, v);
}

static inline int tcc_ir_op_src2_is_vreg(const TCCIRState *ir, const IRQuadCompact *q, int32_t v)
{
  const unsigned cfg = IROP_CFG_BYTE(q->op);
  const uint32_t at = q->operand_base + (cfg & 1) + ((cfg >> 1) & 1);
  return (cfg & 4) && IROP_SLOT_HOLDS(ir, at, v);
}

#define IR_SLOT_HOLDS(slot, ir, q, v, real)                                            \
  ((real) ? tcc_ir_op_##slot##_is_vreg((ir), (q), (v)) : tcc_ir_op_##slot##_vreg((ir), (q)) == (v))

/* irop_get_vreg of an operand slot, without the by-value IROperand round trip at the call site. */
#ifdef TCC_IROP_INLINE_ACCESSORS
static inline int32_t tcc_ir_op_dest_vreg(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_get_vreg(tcc_ir_op_get_dest(ir, q));
}

static inline int32_t tcc_ir_op_src1_vreg(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_get_vreg(tcc_ir_op_get_src1(ir, q));
}

static inline int32_t tcc_ir_op_src2_vreg(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_get_vreg(tcc_ir_op_get_src2(ir, q));
}

/* `pick_src2 ? src2 : src1` and `pick_src1 ? src1 : dest` of an instruction: one call
 * that returns the chosen operand instead of a ternary over two by-value calls (which
 * costs both calls plus a 9-byte copy at every site). */
static inline IROperand tcc_ir_op_get_src1_or_2(const TCCIRState *ir, const IRQuadCompact *q, int pick_src2)
{
  return pick_src2 ? tcc_ir_op_get_src2(ir, q) : tcc_ir_op_get_src1(ir, q);
}

static inline IROperand tcc_ir_op_get_dest_or_src1(const TCCIRState *ir, const IRQuadCompact *q, int pick_src1)
{
  return pick_src1 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_dest(ir, q);
}

/* Operand slot by number: 0 dest, 1 src1, anything else src2. */
static inline IROperand tcc_ir_op_get_slot(const TCCIRState *ir, const IRQuadCompact *q, int slot)
{
  return slot == 0 ? tcc_ir_op_get_dest(ir, q) : slot == 1 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
}

/* irop_has_vreg / irop_get_btype / irop_is_immediate of an operand slot, for the
 * same reason: a temporary that only feeds these never has to exist. */
static inline int tcc_ir_op_dest_has_vreg(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_has_vreg(tcc_ir_op_get_dest(ir, q));
}

static inline int tcc_ir_op_src1_has_vreg(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_has_vreg(tcc_ir_op_get_src1(ir, q));
}

static inline int tcc_ir_op_src2_has_vreg(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_has_vreg(tcc_ir_op_get_src2(ir, q));
}

static inline int tcc_ir_op_dest_btype(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_get_btype(tcc_ir_op_get_dest(ir, q));
}

static inline int tcc_ir_op_src1_btype(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_get_btype(tcc_ir_op_get_src1(ir, q));
}

static inline int tcc_ir_op_src2_btype(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_get_btype(tcc_ir_op_get_src2(ir, q));
}

static inline int32_t tcc_ir_op_dest_imm32(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_get_imm32(tcc_ir_op_get_dest(ir, q));
}

static inline int32_t tcc_ir_op_src2_imm32(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_get_imm32(tcc_ir_op_get_src2(ir, q));
}

static inline int tcc_ir_op_src2_is_none(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_is_none(tcc_ir_op_get_src2(ir, q));
}

static inline int tcc_ir_op_dest_needs_pair(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_needs_pair(tcc_ir_op_get_dest(ir, q));
}

static inline int tcc_ir_op_src2_tag(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_get_tag(tcc_ir_op_get_src2(ir, q));
}

static inline int tcc_ir_op_dest_is_lval(const TCCIRState *ir, const IRQuadCompact *q)
{
  return tcc_ir_op_get_dest(ir, q).is_lval;
}

static inline int tcc_ir_op_src1_is_lval(const TCCIRState *ir, const IRQuadCompact *q)
{
  return tcc_ir_op_get_src1(ir, q).is_lval;
}

static inline int tcc_ir_op_src2_is_lval(const TCCIRState *ir, const IRQuadCompact *q)
{
  return tcc_ir_op_get_src2(ir, q).is_lval;
}

static inline int tcc_ir_op_src1_is_sym(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_is_sym(tcc_ir_op_get_src1(ir, q));
}

static inline int tcc_ir_op_src2_is_sym(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_is_sym(tcc_ir_op_get_src2(ir, q));
}

static inline int tcc_ir_op_dest_is_local(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_is_local(tcc_ir_op_get_dest(ir, q));
}

static inline int tcc_ir_op_src1_is_local(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_is_local(tcc_ir_op_get_src1(ir, q));
}

/* The raw u.imm32 word of the dest slot (a jump target index). */
static inline int32_t tcc_ir_op_dest_u_imm32(const TCCIRState *ir, const IRQuadCompact *q)
{
  return tcc_ir_op_get_dest(ir, q).u.imm32;
}

static inline int tcc_ir_op_src1_is_imm(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_is_immediate(tcc_ir_op_get_src1(ir, q));
}

static inline int tcc_ir_op_src2_is_imm(const TCCIRState *ir, const IRQuadCompact *q)
{
  return irop_is_immediate(tcc_ir_op_get_src2(ir, q));
}
#else
int32_t tcc_ir_op_dest_vreg(const TCCIRState *ir, const IRQuadCompact *q);
int32_t tcc_ir_op_src1_vreg(const TCCIRState *ir, const IRQuadCompact *q);
int32_t tcc_ir_op_src2_vreg(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_dest_has_vreg(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src1_has_vreg(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src2_has_vreg(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_dest_btype(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src1_btype(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src2_btype(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src1_is_imm(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src2_is_imm(const TCCIRState *ir, const IRQuadCompact *q);
int32_t tcc_ir_op_dest_u_imm32(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_dest_is_lval(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src1_is_lval(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src2_is_lval(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src1_is_sym(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src2_is_sym(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_dest_is_local(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src1_is_local(const TCCIRState *ir, const IRQuadCompact *q);
int32_t tcc_ir_op_dest_imm32(const TCCIRState *ir, const IRQuadCompact *q);
int32_t tcc_ir_op_src2_imm32(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src2_is_none(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_dest_needs_pair(const TCCIRState *ir, const IRQuadCompact *q);
int tcc_ir_op_src2_tag(const TCCIRState *ir, const IRQuadCompact *q);
IROperand tcc_ir_op_get_src1_or_2(const TCCIRState *ir, const IRQuadCompact *q, int pick_src2);
IROperand tcc_ir_op_get_dest_or_src1(const TCCIRState *ir, const IRQuadCompact *q, int pick_src1);
IROperand tcc_ir_op_get_slot(const TCCIRState *ir, const IRQuadCompact *q, int slot);
#endif

/* irop_get_imm64_ex of an operand slot (jump targets, condition tokens). */
int64_t tcc_ir_op_dest_imm(const TCCIRState *ir, const IRQuadCompact *q);
int64_t tcc_ir_op_src1_imm(const TCCIRState *ir, const IRQuadCompact *q);
int64_t tcc_ir_op_src2_imm(const TCCIRState *ir, const IRQuadCompact *q);

/* irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q)): a call's callee symbol. */
struct Sym *tcc_ir_op_src1_sym(const TCCIRState *ir, const IRQuadCompact *q);

/* A call the backend emits as instructions that write no register but its
 * result: the DMB of an atomic (__tcc_dmb), an inline read-modify-write
 * (__tcc_ax_*, its loop on scratch registers) and the system instructions of
 * an asm statement (__tcc_mc_*, an mrs into the result's register, an msr from
 * the operand's).  Every pass still treats it as a call that reads and writes
 * memory; the register allocator and the frame do not treat it as one: no
 * value has to leave R0-R3 for it, and it makes no function a non-leaf. */
int tcc_ir_call_clobbers_nothing(const TCCIRState *ir, const IRQuadCompact *q);

/* A call to a noreturn callee, which leaves a function a leaf (-O1, -O2; not
 * -Os, where the per-call push costs more bytes than the prologue's and a leaf
 * loses the machine outliner):
 * the call site pushes LR itself, in the cold block, right before its `bl`
 * (tcc_gen_machine_func_call_mop), so the prologue need not.  ir/codegen.c
 * makes the function non-leaf again when such a call passes stack arguments,
 * or when the prologue pushes other registers anyway. */
int tcc_ir_call_saves_lr_itself(const TCCIRState *ir, const IRQuadCompact *q);

/* Get the 4th operand (scale) for indexed memory operations.
 * This is stored at operand_base + 3 for LOAD_INDEXED/STORE_INDEXED.
 */
IROperand tcc_ir_op_get_scale(const TCCIRState *ir, const IRQuadCompact *q);

/* Get the 4th operand (accumulator) for MLA (Multiply-Accumulate) operations.
 * MLA: dest = src1 * src2 + accum
 * This is stored at operand_base + 3 for MLA.
 */
IROperand tcc_ir_op_get_accum(const TCCIRState *ir, const IRQuadCompact *q);

/* irop_get_vreg(tcc_ir_op_get_accum(ir, q)) */
int32_t tcc_ir_op_accum_vreg(const TCCIRState *ir, const IRQuadCompact *q);

void tcc_ir_op_set_accum(TCCIRState *ir, IRQuadCompact *q, IROperand op);

/* Get the 4th operand (condition code) for SELECT operations.
 * SELECT: dest = (cond) ? src1 : src2
 * Condition is stored at operand_base + 3 as IMM32 (ARM cond nibble).
 */
IROperand tcc_ir_op_get_cond(const TCCIRState *ir, const IRQuadCompact *q);

/* irop_get_imm64_ex(ir, tcc_ir_op_get_cond(ir, q)) */
int64_t tcc_ir_op_cond_imm(const TCCIRState *ir, const IRQuadCompact *q);

/* ============================================================================
 * IROperand pool setter functions
 * ============================================================================
 * Direct operand pool manipulation - used by optimization passes.
 */

void tcc_ir_op_set_dest(TCCIRState *ir, const IRQuadCompact *q, IROperand irop);

void tcc_ir_set_dest(TCCIRState *ir, int index, IROperand irop);

void tcc_ir_op_set_src1(TCCIRState *ir, const IRQuadCompact *q, IROperand irop);

void tcc_ir_set_src1(TCCIRState *ir, int index, IROperand irop);

void tcc_ir_op_set_src2(TCCIRState *ir, const IRQuadCompact *q, IROperand irop);

void tcc_ir_set_src2(TCCIRState *ir, int index, IROperand irop);

/* tcc_ir_set_X(ir, index, IROP_NONE) / tcc_ir_op_set_X(ir, q, IROP_NONE): an
 * operand slot cleared without building IROP_NONE at the call site. */
void tcc_ir_set_dest_none(TCCIRState *ir, int index);
void tcc_ir_set_src1_none(TCCIRState *ir, int index);
void tcc_ir_set_src2_none(TCCIRState *ir, int index);
void tcc_ir_op_set_src1_none(TCCIRState *ir, const IRQuadCompact *q);
void tcc_ir_op_set_src2_none(TCCIRState *ir, const IRQuadCompact *q);

/* tcc_ir_[op_]set_X(..., irop_make_imm32(-1, val, btype)). */
void tcc_ir_set_dest_imm32(TCCIRState *ir, int index, int32_t val, int btype);
void tcc_ir_set_src1_imm32(TCCIRState *ir, int index, int32_t val, int btype);
void tcc_ir_set_src2_imm32(TCCIRState *ir, int index, int32_t val, int btype);
void tcc_ir_op_set_dest_imm32(TCCIRState *ir, const IRQuadCompact *q, int32_t val, int btype);

/* Pool management functions */
int tcc_ir_iroperand_pool_add(TCCIRState *ir, IROperand irop);
