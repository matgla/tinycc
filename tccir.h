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
} TccIrOp;

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
  uint8_t is_complex : 1;      // Phase 3: whether this is a complex type
  uint8_t use_vfp : 1;         // whether to use VFP registers (hard float)
  uint8_t is_lvalue : 1;
  uint8_t crosses_call : 1; // whether interval spans a function call
  uint8_t phi_pinned : 1;   // register relied upon by identity phi — do not reassign
  uint32_t start;           // start instruction index
  uint32_t end;             // end instruction index
  IRVregReplacement allocation;
  int8_t incoming_reg0;    // for params: which register arg arrives in (-1 if stack)
  int8_t incoming_reg1;    // for doubles: second register (-1 if not double or stack)
  int32_t original_offset; // for params: original offset from function entry point
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

/* Compact IR instruction - stores operand indices instead of full SValues */
typedef struct IRQuadCompact
{
  int orig_index;               /* Original IR index (stable across DCE) */
  TccIrOp op;                   /* Operation code */
  uint32_t operand_base;        /* Index into svalue_pool */
  uint32_t line_num : 31;       /* Source line for debug info (non-negative, 31 bits = up to 2B lines) */
  uint32_t is_jump_target : 1;  /* Set when at least one JUMP/JUMPIF targets this instruction */
} IRQuadCompact;

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

typedef struct TCCIRState
{
  // number of function parameters
  int8_t parameters_count;
  /* Named-argument usage for variadic prolog (AAPCS). */
  int named_arg_reg_bytes;
  int named_arg_stack_bytes;

  uint8_t leaffunc : 1;
  uint8_t naked : 1;
  uint8_t processing_if : 1;
  uint8_t check_for_backwards_jumps : 1;
  uint8_t basic_block_start : 1;
  uint8_t prevent_coalescing;
  uint8_t has_static_chain : 1;      /* function uses static chain for nested func */
  uint8_t needs_chain_save : 1;      /* must save chain at FP-4 for multi-hop child access */
  int32_t static_chain_vreg;         /* vreg holding static chain pointer (parent FP) */
  int32_t captured_offsets_list[32]; /* offsets of captured vars (for chain-relative access) */
  int32_t captured_chain_depths[32]; /* 1 = direct R10, 2+ = multi-hop */
  int32_t captured_count;            /* number of captured variables */
  int32_t loc;
  int32_t parent_loc; /* parent's loc value (for nested function offset validation) */

  /* Nested function tracking (for parent functions that contain nested functions) */
  NestedFunc **nested_funcs;     /* array of pointers to nested function descriptors */
  int32_t nb_nested_funcs;       /* count of nested functions */
  int32_t nested_funcs_capacity; /* allocated capacity of nested_funcs array */

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
  int max_orig_index;           /* Highest orig_index ever assigned; updated in tcc_ir_put */
  int next_insn_is_jump_target; /* Pending flag: next tcc_ir_put must set is_jump_target=1 */

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

  LSLiveIntervalState ls;

  /* Extra scratch allocation flags to apply during materialization for the current IR instruction. */
  unsigned codegen_materialize_scratch_flags;

  /* Set between CMP and JUMPIF emission so the backend uses flag-preserving encodings. */
  int codegen_flags_live;

  /* Switch tables for jump table generation */
  TCCIRSwitchTable *switch_tables;
  int num_switch_tables;
  int switch_tables_capacity;

  /* Barrel shift annotations: populated just before codegen, freed after.
   * barrel_shifts[i] encodes an optional barrel shift on src2 of instruction i:
   * 0 = none, else (type<<5)|amount. type: 1=SHL, 2=SHR, 3=SAR, 4=ROR. */
  uint8_t *barrel_shifts;
} TCCIRState;

TCCIRState *tcc_ir_allocate_block();

/* If the value is an lvalue (memory reference), emit an IR load so the
 * SValue becomes a plain value suitable for arithmetic/indirect calls. */

int tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest);

#ifdef CONFIG_TCC_ASM
int tcc_ir_add_inline_asm(TCCIRState *ir, const char *asm_str, int asm_len, int must_subst, ASMOperand *operands,
                          int nb_operands, int nb_outputs, int nb_labels, const uint8_t *clobber_regs);
void tcc_ir_put_inline_asm(TCCIRState *ir, int inline_asm_id);
#endif

int tcc_ir_get_vreg_temp(TCCIRState *ir);
int tcc_ir_get_vreg_var(TCCIRState *ir);
int tcc_ir_get_vreg_param(TCCIRState *ir);
/* Allocate static chain vreg for nested functions (live-in at R10) */
int tcc_ir_get_vreg_static_chain(TCCIRState *ir);

void tcc_ir_set_float_type(TCCIRState *ir, int vreg, int is_float, int is_double);
void tcc_ir_set_llong_type(TCCIRState *ir, int vreg);
void tcc_ir_set_original_offset(TCCIRState *ir, int vreg, int offset);
int tcc_ir_get_reg_type(TCCIRState *ir, int vreg);

void tcc_ir_register_allocation_params(TCCIRState *ir);
/* For parameters that arrive on the caller stack (beyond r0-r3 per AAPCS),
 * do not allocate separate local spill slots. They already have a stable
 * incoming stack home for the duration of the call. */
void tcc_ir_mark_return_value_incoming_regs(TCCIRState *ir);
void tcc_ir_avoid_spilling_stack_passed_params(TCCIRState *ir);
void tcc_ir_build_stack_layout(TCCIRState *ir);
const TCCStackSlot *tcc_ir_stack_slot_by_vreg(const TCCIRState *ir, int vreg);
const TCCStackSlot *tcc_ir_stack_slot_by_offset(const TCCIRState *ir, int frame_offset);
void tcc_ir_assign_physical_register(TCCIRState *ir, int vreg, int offset, int r0, int r1);
const char *tcc_ir_get_op_name(TccIrOp op);
void tcc_ir_show(TCCIRState *ir);
void tcc_ir_dump_set_show_physical_regs(int show);
void tcc_ir_set_addrtaken(TCCIRState *ir, int vreg);

IRLiveInterval *tcc_ir_get_live_interval(TCCIRState *ir, int vreg);
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

/* Check if FPU supports double precision (defined in arm-thumb-gen.c) */
int arm_fpu_supports_double(int fpu_type);

/* SValue pool accessor functions for compact IR storage.
 * Operand layout in pool: dest (if present), src1 (if present), src2 (if present).
 * Returns NULL if the operand is not used by this operation. */

static inline int ir_op_slot_count(TccIrOp op)
{
  return irop_config[op].has_dest + irop_config[op].has_src1 + irop_config[op].has_src2;
}

/* ============================================================================
 * IROperand pool accessor functions - compact 8-byte operand access
 * ============================================================================
 * Operand layout: dest (if present), src1 (if present), src2 (if present).
 * Returns IROP_NONE if the operand is not used by this operation.
 */

static inline IROperand tcc_ir_op_get_dest(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!irop_config[q->op].has_dest)
    return IROP_NONE;
  if (q->operand_base >= (uint32_t)ir->iroperand_pool_count)
    return IROP_NONE;
  return ir->iroperand_pool[q->operand_base];
}

static inline IROperand tcc_ir_get_dest(const TCCIRState *ir, int index)
{
  IRQuadCompact *q = &ir->compact_instructions[index];
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

static inline IROperand tcc_ir_get_src1(const TCCIRState *ir, int index)
{
  IRQuadCompact *q = &ir->compact_instructions[index];
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

static inline IROperand tcc_ir_get_src2(const TCCIRState *ir, int index)
{
  IRQuadCompact *q = &ir->compact_instructions[index];
  if (!irop_config[q->op].has_src2)
    return IROP_NONE;
  int off = irop_config[q->op].has_dest + irop_config[q->op].has_src1;
  if (q->operand_base + off >= (uint32_t)ir->iroperand_pool_count)
    return IROP_NONE;
  return ir->iroperand_pool[q->operand_base + off];
}

/* Get the 4th operand (scale) for indexed memory operations.
 * This is stored at operand_base + 3 for LOAD_INDEXED/STORE_INDEXED.
 */
static inline IROperand tcc_ir_op_get_scale(const TCCIRState *ir, const IRQuadCompact *q)
{
  /* Scale is at operand_base + 3 (after dest, base/src1, index/src2) */
  int scale_idx = q->operand_base + 3;
  if (scale_idx >= 0 && scale_idx < ir->iroperand_pool_count)
    return ir->iroperand_pool[scale_idx];
  return IROP_NONE;
}

/* Get the 4th operand (accumulator) for MLA (Multiply-Accumulate) operations.
 * MLA: dest = src1 * src2 + accum
 * This is stored at operand_base + 3 for MLA.
 */
static inline IROperand tcc_ir_op_get_accum(const TCCIRState *ir, const IRQuadCompact *q)
{
  /* Accumulator is at operand_base + 3 (after dest, src1, src2) */
  int accum_idx = q->operand_base + 3;
  if (accum_idx >= 0 && accum_idx < ir->iroperand_pool_count)
    return ir->iroperand_pool[accum_idx];
  return IROP_NONE;
}

static inline void tcc_ir_op_set_accum(TCCIRState *ir, IRQuadCompact *q, IROperand op)
{
  int accum_idx = q->operand_base + 3;
  if (accum_idx >= 0 && accum_idx < ir->iroperand_pool_count)
    ir->iroperand_pool[accum_idx] = op;
}

/* Get the 4th operand (condition code) for SELECT operations.
 * SELECT: dest = (cond) ? src1 : src2
 * Condition is stored at operand_base + 3 as IMM32 (ARM cond nibble).
 */
static inline IROperand tcc_ir_op_get_cond(const TCCIRState *ir, const IRQuadCompact *q)
{
  int cond_idx = q->operand_base + 3;
  if (cond_idx >= 0 && cond_idx < ir->iroperand_pool_count)
    return ir->iroperand_pool[cond_idx];
  return IROP_NONE;
}

/* ============================================================================
 * IROperand pool setter functions
 * ============================================================================
 * Direct operand pool manipulation - used by optimization passes.
 */

static inline void tcc_ir_op_set_dest(TCCIRState *ir, const IRQuadCompact *q, IROperand irop)
{
  if (!irop_config[q->op].has_dest)
    return;
  ir->iroperand_pool[q->operand_base] = irop;
}

static inline void tcc_ir_set_dest(TCCIRState *ir, int index, IROperand irop)
{
  IRQuadCompact *q = &ir->compact_instructions[index];
  if (!irop_config[q->op].has_dest)
    return;
  ir->iroperand_pool[q->operand_base] = irop;
}

static inline void tcc_ir_op_set_src1(TCCIRState *ir, const IRQuadCompact *q, IROperand irop)
{
  if (!irop_config[q->op].has_src1)
    return;
  int off = irop_config[q->op].has_dest;
  ir->iroperand_pool[q->operand_base + off] = irop;
}

static inline void tcc_ir_set_src1(TCCIRState *ir, int index, IROperand irop)
{
  IRQuadCompact *q = &ir->compact_instructions[index];
  if (!irop_config[q->op].has_src1)
    return;
  int off = irop_config[q->op].has_dest;
  ir->iroperand_pool[q->operand_base + off] = irop;
}

static inline void tcc_ir_op_set_src2(TCCIRState *ir, const IRQuadCompact *q, IROperand irop)
{
  if (!irop_config[q->op].has_src2)
    return;
  int off = irop_config[q->op].has_dest + irop_config[q->op].has_src1;
  ir->iroperand_pool[q->operand_base + off] = irop;
}

static inline void tcc_ir_set_src2(TCCIRState *ir, int index, IROperand irop)
{
  IRQuadCompact *q = &ir->compact_instructions[index];
  if (!irop_config[q->op].has_src2)
    return;
  int off = irop_config[q->op].has_dest + irop_config[q->op].has_src1;
  ir->iroperand_pool[q->operand_base + off] = irop;
}

/* Pool management functions */
int tcc_ir_iroperand_pool_add(TCCIRState *ir, IROperand irop);