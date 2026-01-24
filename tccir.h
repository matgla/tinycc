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

#include "tccls.h"

#define PREG_SPILLED 0x20
#define PREG_NONE 0x1F     /* pr0/pr1 not allocated - just the register bits */
#define PREG_REG_NONE 0x1F /* pr0_reg/pr1_reg not allocated (5-bit field: 31) */

typedef enum TccIrOp : uint8_t
{
  TCCIR_OP_ADD,
  TCCIR_OP_ADC_USE,
  TCCIR_OP_ADC_GEN,
  TCCIR_OP_SUB,
  TCCIR_OP_SUBC_USE,
  TCCIR_OP_SUBC_GEN,
  TCCIR_OP_MUL,
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

  /* No-operation placeholder for dead instructions */
  TCCIR_OP_NOP,
} TccIrOp;

/* FUNCPARAMVAL encoding helpers:
 * src2.c.i encodes both parameter index (lower 16 bits) and call_id (upper 16 bits)
 * This keeps call/param binding explicit and makes the IR more compact.
 */
#define TCCIR_ENCODE_PARAM(call_id, param_idx) (((int64_t)(call_id) << 16) | ((param_idx) & 0xFFFF))
#define TCCIR_DECODE_CALL_ID(encoded) ((int)((encoded) >> 16))
#define TCCIR_DECODE_PARAM_IDX(encoded) ((int)((encoded) & 0xFFFF))

/* FUNCCALL encoding helpers:
 * For FUNCCALLVOID/FUNCCALLVAL, src2.c.i encodes call_id (bits 16-31) and argc (bits 0-15).
 * This allows the backend to know how many arguments to expect without scanning.
 */
#define TCCIR_ENCODE_CALL(call_id, argc) (((int64_t)(call_id) << 16) | ((argc) & 0xFFFF))
#define TCCIR_DECODE_CALL_ARGC(encoded) ((int)((encoded) & 0xFFFF))

typedef struct CType CType;
typedef struct SValue SValue;

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
  uint16_t r0; // first physical register
  uint16_t r1; // second physical register (for long long)
  int offset;  // stack offset if spilled
} IRVregReplacement;

typedef struct IRLiveInterval
{
  uint8_t start_within_if : 1; // whether the interval starts within an if block
  uint8_t addrtaken : 1;       // whether the variable's address is taken
  uint8_t is_float : 1;        // whether this is a float/double variable
  uint8_t is_double : 1;       // whether this is a double (vs float)
  uint8_t is_llong : 1;        // whether this is a long long (64-bit int)
  uint8_t use_vfp : 1;         // whether to use VFP registers (hard float)
  uint8_t is_lvalue : 1;
  uint8_t crosses_call : 1; // whether interval spans a function call
  uint32_t start;           // start instruction index
  uint32_t end;             // end instruction index
  IRVregReplacement allocation;
  int8_t incoming_reg0;    // for params: which register arg arrives in (-1 if stack)
  int8_t incoming_reg1;    // for doubles: second register (-1 if not double or stack)
  int16_t original_offset; // for params: original offset from function entry point
  int stack_slot_index;    // index into stack layout (-1 if not stack-backed)
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
  uint8_t live_across_calls;
  uint8_t addressable; // non-zero if slot must remain addressable (addr taken)
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
} TCCStackLayout;

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
/* Exclude "permanent scratch" regs (e.g. R11/R12 on ARM) from scratch allocation. */
#define TCC_MACHINE_SCRATCH_AVOID_PERM_SCRATCH (1u << 4)

typedef struct TCCMaterializedValue
{
  uint8_t used_scratch;
  uint8_t is_64bit;
  uint8_t original_pr0;
  uint8_t original_pr1;
  unsigned short original_r;
  uint64_t original_c_i;
  TCCMachineScratchRegs scratch;
} TCCMaterializedValue;

typedef struct TCCMaterializedAddr
{
  uint8_t used_scratch;
  uint8_t original_pr0;
  uint8_t original_pr1;
  unsigned short original_r;
  uint64_t original_c_i;
  TCCMachineScratchRegs scratch;
} TCCMaterializedAddr;

typedef struct TCCMaterializedDest
{
  uint8_t needs_storeback;
  uint8_t is_64bit;
  uint8_t original_pr0;
  uint8_t original_pr1;
  unsigned short original_r;
  int frame_offset;
  TCCMachineScratchRegs scratch;
} TCCMaterializedDest;

/* Compact IR instruction - stores operand indices instead of full SValues */
typedef struct IRQuadCompact
{
  int orig_index;        /* Original IR index (stable across DCE) */
  TccIrOp op;            /* Operation code */
  uint32_t operand_base; /* Index into svalue_pool */
  int line_num;          /* Source line for debug info */
} IRQuadCompact;

/* Per-operation operand configuration (defined in tccir.c) */
typedef struct IRRegistersConfig
{
  uint8_t has_dest : 1;
  uint8_t has_src1 : 1;
  uint8_t has_src2 : 1;
} IRRegistersConfig;

extern const IRRegistersConfig irop_config[];

/* ============================================================================
 * IROperand: Compact 8-byte operand representation (vs ~56 byte SValue)
 * ============================================================================
 * Always includes vreg field so optimization passes can access it directly.
 * Tag and flags are packed into reserved bits of the vr field.
 *
 * Memory savings: 8 bytes vs 56 bytes = ~7x smaller per operand.
 * Main savings come from:
 *   - No CType struct (8+ bytes)
 *   - No full CValue union
 *   - Pool indices for 64-bit values and symbols
 *   - Tag+flags packed into vr field
 *
 * vr field layout (32 bits):
 *   Bits 0-19:  vreg position (max 1M vregs per type - plenty for 131K observed)
 *   Bits 20-22: tag (3 bits, 8 values)
 *   Bits 23-24: flags (2 bits)
 *   Bits 25-27: reserved
 *   Bits 28-31: vreg type (from TCCIR_ENCODE_VREG)
 *
 * Special case: vr == -1 (0xFFFFFFFF) means "no vreg associated"
 */

/* Bit positions for tag/flags in vr field */
#define IROP_VR_TAG_SHIFT 20
#define IROP_VR_TAG_MASK (0x7 << IROP_VR_TAG_SHIFT) /* 3 bits */
#define IROP_VR_FLAGS_SHIFT 23
#define IROP_VR_FLAGS_MASK (0x3 << IROP_VR_FLAGS_SHIFT) /* 2 bits */
#define IROP_VR_POSITION_MASK 0xFFFFF                   /* 20 bits for position */

/* Tags for IROperand (stored in bits 20-22 of vr) */
#define IROP_TAG_NONE 0     /* sentinel for unused operand */
#define IROP_TAG_VREG 1     /* pure vreg with no additional data */
#define IROP_TAG_IMM32 2    /* payload.imm32: signed 32-bit immediate */
#define IROP_TAG_STACKOFF 3 /* payload.imm32: signed 32-bit FP-relative offset */
#define IROP_TAG_F32 4      /* payload.f32_bits: 32-bit float bits (inline) */
#define IROP_TAG_I64 5      /* payload.pool_idx: index into pool_i64[] */
#define IROP_TAG_F64 6      /* payload.pool_idx: index into pool_f64[] */
#define IROP_TAG_SYMREF 7   /* payload.pool_idx: index into pool_symref[] */

/* Flags for IROperand (stored in bits 23-24 of vr) */
#define IROP_FLAG_LVAL (1u << 0)  /* value is an lvalue (needs dereference) */
#define IROP_FLAG_LOCAL (1u << 1) /* VT_LOCAL semantics */

typedef struct __attribute__((packed)) IROperand
{
  int32_t vr; /* vreg id with embedded tag+flags, -1 if not associated */
  union
  {
    int32_t imm32;     /* for IMM32, STACKOFF */
    uint32_t f32_bits; /* for F32 */
    uint32_t pool_idx; /* for I64, F64, SYMREF */
  } u;
} IROperand;

_Static_assert(sizeof(IROperand) == 8, "IROperand must be 8 bytes");

/* Extract tag from vr field (handles vr == -1 case) */
static inline int irop_get_tag(int32_t vr)
{
  if (vr < 0)
    return IROP_TAG_NONE;
  return (vr & IROP_VR_TAG_MASK) >> IROP_VR_TAG_SHIFT;
}

/* Extract flags from vr field (handles vr == -1 case) */
static inline int irop_get_flags(int32_t vr)
{
  if (vr < 0)
    return 0;
  return (vr & IROP_VR_FLAGS_MASK) >> IROP_VR_FLAGS_SHIFT;
}

/* Extract clean vreg value (strips tag+flags, preserves type bits) */
static inline int32_t irop_get_vreg(int32_t vr)
{
  if (vr < 0)
    return -1;
  /* Keep type bits (28-31) and position bits (0-19), clear tag/flags (20-27) */
  return (vr & 0xF00FFFFF);
}

/* Encode vreg with tag and flags */
static inline int32_t irop_encode_vr(int32_t vreg, int tag, int flags)
{
  if (vreg < 0)
  {
    /* No vreg - encode tag/flags in a special way or just return -1 with embedded info */
    /* For vr == -1, we can't embed info, so we use a sentinel approach:
     * Use a large negative value that encodes tag/flags in low bits */
    return -1; /* For now, just return -1; tag/flags need payload inspection */
  }
  /* Clear existing tag/flags bits, then set new ones */
  int32_t clean = vreg & ~(IROP_VR_TAG_MASK | IROP_VR_FLAGS_MASK);
  return clean | (tag << IROP_VR_TAG_SHIFT) | (flags << IROP_VR_FLAGS_SHIFT);
}

/* Sentinel for "no operand" */
#define IROP_NONE ((IROperand){.vr = -1, .u = {.imm32 = 0}})

/* Encoding helpers */
static inline IROperand irop_make_none(void)
{
  IROperand op;
  op.vr = -1;
  op.u.imm32 = 0;
  return op;
}

static inline IROperand irop_make_vreg(int32_t vreg)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_VREG, 0);
  op.u.imm32 = 0;
  return op;
}

static inline IROperand irop_make_imm32(int32_t vreg, int32_t val)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_IMM32, 0);
  op.u.imm32 = val;
  return op;
}

static inline IROperand irop_make_stackoff(int32_t vreg, int32_t offset, uint8_t flags)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_STACKOFF, flags);
  op.u.imm32 = offset;
  return op;
}

static inline IROperand irop_make_f32(int32_t vreg, uint32_t bits)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_F32, 0);
  op.u.f32_bits = bits;
  return op;
}

static inline IROperand irop_make_i64(int32_t vreg, uint32_t pool_idx)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_I64, 0);
  op.u.pool_idx = pool_idx;
  return op;
}

static inline IROperand irop_make_f64(int32_t vreg, uint32_t pool_idx)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_F64, 0);
  op.u.pool_idx = pool_idx;
  return op;
}

static inline IROperand irop_make_symref(int32_t vreg, uint32_t pool_idx, uint8_t flags)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_SYMREF, flags);
  op.u.pool_idx = pool_idx;
  return op;
}

/* Decoding helpers */
static inline int irop_is_none(IROperand op)
{
  return op.vr < 0 || irop_get_tag(op.vr) == IROP_TAG_NONE;
}

static inline int irop_has_vreg(IROperand op)
{
  return op.vr >= 0;
}

/* ============================================================================
 * Pool entry types - separate arrays for cache efficiency
 * ============================================================================
 */

/* Symref pool entry: symbol reference with addend and flags */
#define IRPOOL_SYMREF_LVAL (1u << 0)  /* value is an lvalue (needs dereference) */
#define IRPOOL_SYMREF_LOCAL (1u << 1) /* VT_LOCAL semantics */

typedef struct IRPoolSymref
{
  struct Sym *sym;
  int32_t addend;
  uint32_t flags;
} IRPoolSymref;

typedef struct TCCIRState
{
  // number of function parameters
  int8_t parameters_count;
  /* Named-argument usage for variadic prolog (AAPCS). */
  int named_arg_reg_bytes;
  int named_arg_stack_bytes;

  uint8_t leaffunc : 1;
  uint8_t processing_if : 1;
  uint8_t check_for_backwards_jumps : 1;
  uint8_t basic_block_start : 1;
  uint8_t prevent_coalescing;
  int32_t loc;

  /* SValue pool for compact IR storage - operands stored contiguously */
  SValue *svalue_pool;
  int svalue_pool_count;
  int svalue_pool_capacity;

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

  /* IROperand array - parallel to svalue_pool, stores compact 8-byte operands.
   * Index i in iroperand_pool corresponds to index i in svalue_pool.
   * During migration, both are populated; after migration, svalue_pool is removed. */
  IROperand *iroperand_pool;
  int iroperand_pool_count;
  int iroperand_pool_capacity;

  /* Compact instruction array - parallel to instructions[] for now */
  IRQuadCompact *compact_instructions;
  int compact_instructions_size;

  IRLiveInterval **active_set;

  IRLiveInterval *variables_live_intervals;
  int variables_live_intervals_size;
  int next_local_variable;

  IRLiveInterval *temporary_variables_live_intervals;
  int temporary_variables_live_intervals_size;
  int next_temporary_variable;

  IRLiveInterval *parameters_live_intervals;
  int parameters_live_intervals_size;
  int next_parameter;

  int next_live_interval_index;
  int instructions_size;
  int next_instruction_index;

  /* Monotonic ID for binding FUNCPARAM* instructions to their owning FUNCCALL*.
   * Encoded in instruction operands for those ops.
   * 0 means "legacy/unknown" and falls back to nested-scan binding.
   */
  int next_call_id;

  /* Current instruction index during code generation - used for scratch register allocation */
  int codegen_instruction_idx;

  /* Outgoing call argument area reserved in the function frame (FP-relative).
   * If non-zero, stack args are stored at [FP + call_outgoing_base + stack_off].
   */
  int call_outgoing_base; /* frame offset (typically negative) */
  int call_outgoing_size; /* bytes reserved (may include alignment padding) */

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
   * This is populated during tcc_ir_generate_code() and is used after codegen
   * for features like GCC's labels-as-values (&&label). */
  uint32_t *ir_to_code_mapping;
  int ir_to_code_mapping_size;

  /* Mapping from ORIGINAL IR instruction index (pre-DCE/compaction) to generated
   * machine code offset. Label positions (s->jind) are recorded before DCE, so
   * this mapping is the correct one to use for &&label materialization.
   */
  uint32_t *orig_ir_to_code_mapping;
  int orig_ir_to_code_mapping_size;

  LSLiveIntervalState ls;

  /* Extra scratch allocation flags to apply during materialization for the current IR instruction. */
  unsigned codegen_materialize_scratch_flags;
} TCCIRState;

TCCIRState *tcc_ir_allocate_block();
void tcc_ir_release_block(TCCIRState *ir);

/* If the value is an lvalue (memory reference), emit an IR load so the
 * SValue becomes a plain value suitable for arithmetic/indirect calls. */

void tcc_ir_add_function_parameters(TCCIRState *ir, CType *func_type);

int tcc_ir_gvtst(TCCIRState *ir, int inv, int t);

void tcc_ir_gen_opi(TCCIRState *ir, int op);
void tcc_ir_gen_opf(TCCIRState *ir, int op);
int tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest);

#ifdef CONFIG_TCC_ASM
int tcc_ir_add_inline_asm(TCCIRState *ir, const char *asm_str, int asm_len, int must_subst, ASMOperand *operands,
                          int nb_operands, int nb_outputs, int nb_labels, const uint8_t *clobber_regs);
void tcc_ir_put_inline_asm(TCCIRState *ir, int inline_asm_id);
#endif

int tcc_ir_get_vreg_temp(TCCIRState *ir);
int tcc_ir_get_vreg_var(TCCIRState *ir);
int tcc_ir_get_vreg_param(TCCIRState *ir);

void tcc_ir_set_float_type(TCCIRState *ir, int vreg, int is_float, int is_double);
void tcc_ir_set_llong_type(TCCIRState *ir, int vreg);
void tcc_ir_set_original_offset(TCCIRState *ir, int vreg, int offset);
int tcc_ir_get_reg_type(TCCIRState *ir, int vreg);

void tcc_ir_liveness_analysis(TCCIRState *ir);
void tcc_ir_register_allocation_params(TCCIRState *ir);
/* For parameters that arrive on the caller stack (beyond r0-r3 per AAPCS),
 * do not allocate separate local spill slots. They already have a stable
 * incoming stack home for the duration of the call. */
void tcc_ir_mark_return_value_incoming_regs(TCCIRState *ir);
void tcc_ir_avoid_spilling_stack_passed_params(TCCIRState *ir);
void tcc_ir_generate_code(TCCIRState *ir);
void tcc_ir_build_stack_layout(TCCIRState *ir);
const TCCStackSlot *tcc_ir_stack_slot_by_vreg(const TCCIRState *ir, int vreg);
const TCCStackSlot *tcc_ir_stack_slot_by_offset(const TCCIRState *ir, int frame_offset);
void tcc_ir_materialize_value(TCCIRState *ir, SValue *sv, TCCMaterializedValue *result);
void tcc_ir_materialize_const_to_reg(TCCIRState *ir, SValue *sv, TCCMaterializedValue *result);
void tcc_ir_materialize_addr(TCCIRState *ir, SValue *sv, TCCMaterializedAddr *result, int dest_reg);
void tcc_ir_materialize_dest(TCCIRState *ir, SValue *dest, TCCMaterializedDest *result);

int tcc_ir_add_local_variable(TCCIRState *ir, Sym *sym, int stack_offset);
void tcc_ir_assign_physical_register(TCCIRState *ir, int vreg, int offset, int r0, int r1);
const char *tcc_ir_get_op_name(TccIrOp op);
void tcc_ir_show(TCCIRState *ir);
void tcc_ir_drop_return_value(TCCIRState *ir);
void tcc_ir_set_addrtaken(TCCIRState *ir, int vreg);

void tcc_ir_patch_live_intervals_registers(TCCIRState *ir);
IRLiveInterval *tcc_ir_get_live_interval(TCCIRState *ir, int vreg);
void tcc_ir_backpatch(TCCIRState *ir, int t, int target_address);
void tcc_ir_backpatch_to_here(TCCIRState *ir, int t);
void tcc_ir_backpatch_first(TCCIRState *ir, int t, int target_address);
int tcc_ir_gjmp_append(TCCIRState *ir, int n, int t);
int tcc_ir_generate_test(TCCIRState *ir, int inv, int t);
int tcc_ir_dead_code_elimination(TCCIRState *ir);
int tcc_ir_dead_store_elimination(TCCIRState *ir);
int tcc_ir_constant_propagation(TCCIRState *ir);
int tcc_ir_tmp_constant_propagation(TCCIRState *ir);
int tcc_ir_copy_propagation(TCCIRState *ir);
int tcc_ir_arithmetic_cse(TCCIRState *ir);
int tcc_ir_bool_cse(TCCIRState *ir);
int tcc_ir_bool_idempotent(TCCIRState *ir);
int tcc_ir_bool_simplification(TCCIRState *ir);
int tcc_ir_return_value_optimization(TCCIRState *ir);
int tcc_ir_store_load_forwarding(TCCIRState *ir);
int tcc_ir_redundant_store_elimination(TCCIRState *ir);
void tcc_ir_print_vreg(int vreg);
void tcc_ir_generate_cmp_jmp_set(TCCIRState *ir);
void tcc_ir_start_basic_block(TCCIRState *ir);

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

/* Check if FPU supports double precision (defined in arm-thumb-gen.c) */
int arm_fpu_supports_double(int fpu_type);

typedef enum TCCIR_VREG_TYPE
{
  TCCIR_VREG_TYPE_VAR = 1,
  TCCIR_VREG_TYPE_TEMP = 2,
  TCCIR_VREG_TYPE_PARAM = 3,
} TCCIR_VREG_TYPE;

/* Vreg encoding: type in top 4 bits, position in bottom 20 bits.
 * Bits 20-27 are reserved for IROperand tag+flags encoding. */
#define TCCIR_VREG_POSITION_MASK 0xFFFFF /* 20 bits for position */
#define TCCIR_DECODE_VREG_POSITION(vr) ((vr) & TCCIR_VREG_POSITION_MASK)
#define TCCIR_DECODE_VREG_TYPE(vr) ((vr) >> 28)
#define TCCIR_ENCODE_VREG(type, position) (((type) << 28) | ((position) & TCCIR_VREG_POSITION_MASK))

/* SValue pool accessor functions for compact IR storage.
 * Operand layout in pool: dest (if present), src1 (if present), src2 (if present).
 * Returns NULL if the operand is not used by this operation. */

static inline int ir_op_slot_count(TccIrOp op)
{
  return irop_config[op].has_dest + irop_config[op].has_src1 + irop_config[op].has_src2;
}

static inline SValue *tcc_ir_op_get_dest(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!irop_config[q->op].has_dest)
    return NULL;
  return &ir->svalue_pool[q->operand_base];
}

static inline SValue *tcc_ir_get_dest(const TCCIRState *ir, int index)
{
  IRQuadCompact *q = &ir->compact_instructions[index];
  if (!irop_config[q->op].has_dest)
    return NULL;
  return &ir->svalue_pool[q->operand_base];
}

static inline SValue *tcc_ir_op_get_src1(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!irop_config[q->op].has_src1)
    return NULL;
  int off = irop_config[q->op].has_dest;
  return &ir->svalue_pool[q->operand_base + off];
}

static inline SValue *tcc_ir_get_src1(const TCCIRState *ir, int index)
{
  IRQuadCompact *q = &ir->compact_instructions[index];
  if (!irop_config[q->op].has_src1)
    return NULL;
  const int off = irop_config[q->op].has_dest;
  return &ir->svalue_pool[q->operand_base + off];
}

static inline SValue *tcc_ir_op_get_src2(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!irop_config[q->op].has_src2)
    return NULL;
  int off = irop_config[q->op].has_dest + irop_config[q->op].has_src1;
  return &ir->svalue_pool[q->operand_base + off];
}

static inline SValue *tcc_ir_get_src2(const TCCIRState *ir, int index)
{
  IRQuadCompact *q = &ir->compact_instructions[index];
  if (!irop_config[q->op].has_src2)
    return NULL;
  int off = irop_config[q->op].has_dest + irop_config[q->op].has_src1;
  return &ir->svalue_pool[q->operand_base + off];
}

/* ============================================================================
 * IROperand pool accessor functions - compact 8-byte operand access
 * ============================================================================
 * These mirror the SValue accessors but return IROperand instead.
 * Operand layout matches svalue_pool: dest (if present), src1, src2.
 * Returns IROP_NONE if the operand is not used by this operation.
 */

static inline IROperand tcc_ir_op_get_dest_irop(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!irop_config[q->op].has_dest)
    return IROP_NONE;
  return ir->iroperand_pool[q->operand_base];
}

static inline IROperand tcc_ir_get_dest_irop(const TCCIRState *ir, int index)
{
  IRQuadCompact *q = &ir->compact_instructions[index];
  if (!irop_config[q->op].has_dest)
    return IROP_NONE;
  return ir->iroperand_pool[q->operand_base];
}

static inline IROperand tcc_ir_op_get_src1_irop(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!irop_config[q->op].has_src1)
    return IROP_NONE;
  int off = irop_config[q->op].has_dest;
  return ir->iroperand_pool[q->operand_base + off];
}

static inline IROperand tcc_ir_get_src1_irop(const TCCIRState *ir, int index)
{
  IRQuadCompact *q = &ir->compact_instructions[index];
  if (!irop_config[q->op].has_src1)
    return IROP_NONE;
  int off = irop_config[q->op].has_dest;
  return ir->iroperand_pool[q->operand_base + off];
}

static inline IROperand tcc_ir_op_get_src2_irop(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!irop_config[q->op].has_src2)
    return IROP_NONE;
  int off = irop_config[q->op].has_dest + irop_config[q->op].has_src1;
  return ir->iroperand_pool[q->operand_base + off];
}

static inline IROperand tcc_ir_get_src2_irop(const TCCIRState *ir, int index)
{
  IRQuadCompact *q = &ir->compact_instructions[index];
  if (!irop_config[q->op].has_src2)
    return IROP_NONE;
  int off = irop_config[q->op].has_dest + irop_config[q->op].has_src1;
  return ir->iroperand_pool[q->operand_base + off];
}

/* Pool management functions */
void tcc_ir_svalue_pool_init(TCCIRState *ir);
void tcc_ir_svalue_pool_free(TCCIRState *ir);
int tcc_ir_svalue_pool_add(TCCIRState *ir, const SValue *sv);

/* IROperand pool management - separate pools for cache efficiency */
void tcc_ir_pools_init(TCCIRState *ir);
void tcc_ir_pools_free(TCCIRState *ir);
uint32_t tcc_ir_pool_add_i64(TCCIRState *ir, int64_t val);
uint32_t tcc_ir_pool_add_f64(TCCIRState *ir, uint64_t bits);
uint32_t tcc_ir_pool_add_symref(TCCIRState *ir, struct Sym *sym, int32_t addend, uint32_t flags);

/* IROperand <-> SValue conversion functions */
IROperand svalue_to_iroperand(TCCIRState *ir, const SValue *sv);
void iroperand_to_svalue(const TCCIRState *ir, IROperand op, SValue *out);

/* Expand a compact instruction to a full TACQuadruple (for migration) */
void tcc_ir_expand_quad(TCCIRState *ir, int index, TACQuadruple *out);

/* Write back modified operands from a TACQuadruple to the pool */
void tcc_ir_writeback_quad(TCCIRState *ir, int index, TACQuadruple *q);

/* Synchronized write helpers - update BOTH svalue_pool and IROperand pools */
void tcc_ir_sync_operand(TCCIRState *ir, int instr_idx, int operand_slot, const SValue *sv);
void tcc_ir_sync_quad(TCCIRState *ir, int instr_idx, const TACQuadruple *q);