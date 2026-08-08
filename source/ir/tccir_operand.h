#pragma once

#include <stdint.h>
#include <stdlib.h>

struct Sym;
struct TCCIRState;
struct SValue;
struct CType;

/* ============================================================================
 * Vreg encoding
 * ============================================================================
 * Vreg encoding: type in top 4 bits, position in bottom 17 bits.
 * Bits 17-27 are used for IROperand tag+flags+btype encoding.
 *
 * 17 bits for position = 131,072 max vregs (plenty for any function)
 */

typedef enum TCCIR_VREG_TYPE
{
  TCCIR_VREG_TYPE_VAR = 1,
  TCCIR_VREG_TYPE_TEMP = 2,
  TCCIR_VREG_TYPE_PARAM = 3,
} TCCIR_VREG_TYPE;

#define TCCIR_VREG_POSITION_MASK 0x1FFFF /* 17 bits for position */
#define TCCIR_DECODE_VREG_POSITION(vr) ((vr) & TCCIR_VREG_POSITION_MASK)
#define TCCIR_DECODE_VREG_TYPE(vr) ((vr) >> 28)
#define TCCIR_ENCODE_VREG(type, position) (((type) << 28) | ((position) & TCCIR_VREG_POSITION_MASK))

/* ============================================================================
 * IROperand: Compact 10-byte operand representation (vs ~56 byte SValue)
 * ============================================================================
 * Always includes vreg field so optimization passes can access it directly.
 * Tag, flags, and btype are packed into the vr field.
 *
 * vr field layout (32 bits):
 *   Bits 0-17:  vreg position (18 bits, max 262K vregs)
 *   Bits 18-20: tag (3 bits) - IROP_TAG_*
 *   Bit 21:     is_lval - value is an lvalue (needs dereference)
 *   Bit 22:     is_llocal - VT_LLOCAL semantics (double indirection)
 *   Bit 23:     is_local - VT_LOCAL semantics
 *   Bit 24:     is_const - VT_CONST semantics
 *   Bits 25-27: btype (3 bits) - IROP_BTYPE_*
 *   Bits 28-31: vreg type (4 bits) - TCCIR_VREG_TYPE_*
 *
 * Special case: vr == -1 (0xFFFFFFFF) means "no vreg associated".
 */

/* Tags for IROperand (stored in bits 18-20 of vr) */
#define IROP_TAG_NONE 0     /* sentinel for unused operand */
#define IROP_TAG_VREG 1     /* pure vreg with no additional data */
#define IROP_TAG_IMM32 2    /* payload.imm32: signed 32-bit immediate */
#define IROP_TAG_STACKOFF 3 /* payload.imm32: signed 32-bit FP-relative offset
                               *
                               * IMPORTANT: not every STACKOFF operand is a real
                               * stack slot reference.  A *direct* stack location
                               * has tag == STACKOFF, is_local == 1, is_lval == 1
                               * AND vreg_type == 0.  When a VAR or PARAM is
                               * referenced via its potential spill encoding,
                               * vreg_type is non-zero and the offset field is
                               * only metadata about where it *would* spill; the
                               * program reads from the vreg, not from that slot.
                               * New passes that inspect stack operands MUST
                               * check vreg_type == 0 to avoid miscompiles. */
#define IROP_TAG_F32 4      /* payload.f32_bits: 32-bit float bits (inline) */
#define IROP_TAG_I64 5      /* payload.pool_idx: index into pool_i64[] */
#define IROP_TAG_F64 6      /* payload.pool_idx: index into pool_f64[] */
#define IROP_TAG_SYMREF 7   /* payload.pool_idx: index into pool_symref[] */

/* For IROP_TAG_VREG operands with vreg=-1: u.imm32 encodes a pinned physical
 * register.  Bit 8 is the validity flag; bits 0-4 hold the ARM register number
 * (0-15).  When bit 8 is clear (u.imm32 == 0, the irop_make_vreg default), no
 * physical register is pinned.  machine_op_from_ir() and irop_phys_r0() read
 * this encoding. */
#define IROP_VREG_PHYS_VALID 0x100u /* validity flag for pinned phys reg */
#define IROP_VREG_PHYS_MASK 0x1Fu   /* bits 0-4: register number */

/* Sentinel for negative vreg encoding - upper 13 bits of position all set */
#define IROP_NEG_VREG_SENTINEL 0x1FFF0 /* position bits 4-16 all set, bits 0-3 hold neg index */

/* Compressed basic type (stored in bits 25-27 of vr)
 * This allows reconstruction of type.t during iroperand_to_svalue().
 * Preserves byte/short distinction for correct load instruction generation. */
#define IROP_BTYPE_INT32 0   /* VT_VOID, VT_INT, VT_PTR */
#define IROP_BTYPE_INT64 1   /* VT_LLONG */
#define IROP_BTYPE_FLOAT32 2 /* VT_FLOAT */
#define IROP_BTYPE_FLOAT64 3 /* VT_DOUBLE, VT_LDOUBLE */
#define IROP_BTYPE_STRUCT 4  /* VT_STRUCT */
#define IROP_BTYPE_FUNC 5    /* VT_FUNC */
#define IROP_BTYPE_INT8 6    /* VT_BYTE */
#define IROP_BTYPE_INT16 7   /* VT_SHORT */

typedef struct __attribute__((packed)) IROperand
{
  /* vreg id with embedded tag+flags+btype, -1 if not associated */
  union
  {
    int32_t vr; /* raw access for encoding/decoding */
    struct
    {
      uint32_t position : 17;  /* vreg position (0-16) */
      uint32_t is_complex : 1; /* DONE: Phase 2 - VT_COMPLEX: complex type flag (17) */
      uint32_t tag : 3;        /* IROP_TAG_* (18-20) */
      uint32_t is_lval : 1;    /* VT_LVAL: needs dereference (21) */
      uint32_t is_llocal : 1;  /* VT_LLOCAL: double indirection (22) */
      uint32_t is_local : 1;   /* VT_LOCAL: stack-relative (23) */
      uint32_t is_const : 1;   /* VT_CONST: constant value (24) */
      uint32_t btype : 3;      /* IROP_BTYPE_* (25-27) */
      uint32_t vreg_type : 4;  /* TCCIR_VREG_TYPE_* (28-31).
                                  For IROP_TAG_STACKOFF: zero means a real
                                  direct StackLoc reference; non-zero means a
                                  vreg-backed spill encoding (see above). */
    };
  };
  union
  {
    int32_t imm32;     /* for IMM32, STACKOFF (non-struct) */
    uint32_t f32_bits; /* for F32 */
    uint32_t pool_idx; /* for I64, F64, SYMREF (non-struct) */
    struct
    {                     /* for STRUCT types - split encoding */
      uint16_t ctype_idx; /* index into pool_ctype (lower 16 bits) */
      int16_t aux_data;   /* aux: stack offset for STACKOFF, symref_idx for SYMREF */
    } s;
  } u;
  /* Type flags (filled during IR construction) */
  uint8_t is_unsigned : 1; /* VT_UNSIGNED flag */
  uint8_t is_static : 1;   /* VT_STATIC flag */
  uint8_t is_sym : 1;      /* VT_SYM: has associated symbol */
  uint8_t is_param : 1;    /* VT_PARAM: stack-passed parameter (needs offset_to_args) */
  uint8_t aux : 4;         /* IROP_AUX_* flag bits (see below).  Deliberately ONE
                            * 4-bit member accessed via masks, not individual
                            * 1-bit members: growing this struct's bitfield
                            * member count makes host GCC stop scalarizing the
                            * packed 9-byte struct in by-value copies, which
                            * measurably tripled whole-IR scan passes (vrp on
                            * tests2/101_cleanup went 19s -> 56s). */
} IROperand;

_Static_assert(sizeof(IROperand) == 9, "IROperand must be 9 bytes");

/* IROperand.aux flag bits. */
#define IROP_AUX_ALIGN4_OK 0x1u   /* 64-bit lvalue only: the accessed address is
                                   * proven >= 4-byte aligned (static type rules,
                                   * no packed member in the access chain), so the
                                   * backend may use LDRD/STRD through a general
                                   * base register.  Default clear = not proven;
                                   * operands built by IR passes stay clear and
                                   * fall back to the unaligned-safe LDR/STR pair. */
#define IROP_AUX_UNDERALIGN 0x2u  /* The access chain crossed a packed member, so
                                   * the address may be < 4-byte aligned.
                                   * Inverse-polarity sibling of ALIGN4_OK for the
                                   * LOAD_INDEXED / STORE_INDEXED path, whose
                                   * 64-bit lowering has historically assumed
                                   * alignment: fusion passes copy this from the
                                   * original deref operand onto the base operand,
                                   * and the backend then avoids LDRD/STRD.
                                   * Default clear keeps legacy indexed behavior. */

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

/* IROperand pool management - separate pools for cache efficiency */
void tcc_ir_pools_init(struct TCCIRState *ir);
void tcc_ir_pools_free(struct TCCIRState *ir);
uint32_t tcc_ir_pool_add_i64(struct TCCIRState *ir, int64_t val);
uint32_t tcc_ir_pool_add_f64(struct TCCIRState *ir, uint64_t bits);
uint32_t tcc_ir_pool_add_symref(struct TCCIRState *ir, struct Sym *sym, int32_t addend, uint32_t flags);
uint32_t tcc_ir_pool_add_ctype(struct TCCIRState *ir, const struct CType *ctype);

/* Pool read accessors (for inline helpers) */
int64_t *tcc_ir_pool_get_i64_ptr(const struct TCCIRState *ir, uint32_t idx);
uint64_t *tcc_ir_pool_get_f64_ptr(const struct TCCIRState *ir, uint32_t idx);
IRPoolSymref *tcc_ir_pool_get_symref_ptr(const struct TCCIRState *ir, uint32_t idx);
struct CType *tcc_ir_pool_get_ctype_ptr(const struct TCCIRState *ir, uint32_t idx);
struct Sym *irop_get_sym(IROperand op);

/* IROperand <-> SValue conversion functions */
IROperand svalue_to_iroperand(struct TCCIRState *ir, const struct SValue *sv);
void iroperand_to_svalue(const struct TCCIRState *ir, IROperand op, struct SValue *out);

/* Convert IROP_BTYPE to VT_BTYPE */
int irop_btype_to_vt_btype(int irop_btype);

/* Type size/alignment from IROperand (uses CType pool for structs) */
int irop_type_size(IROperand op);
int irop_type_size_align(IROperand op, int *align_out);

/* AAPCS natural alignment for parameter passing (walks struct members,
 * ignoring __attribute__((aligned)) on the struct itself). */
int irop_aapcs_alignment(IROperand op);

/* AAPCS natural alignment from CType (for callee-side parameter layout). */
int ctype_aapcs_alignment(struct CType *ct);

/* Get CType for struct operands (returns NULL for non-struct types) */
struct CType *irop_get_ctype(IROperand op);

/* Debug: compare SValue with IROperand and print differences (returns 1 if mismatch) */
int irop_compare_svalue(const struct TCCIRState *ir, const struct SValue *sv, IROperand op, const char *context);

/* Position sentinel value: max 17-bit value means "no position" */
#define IROP_POSITION_NONE 0x1FFFF

/* Inlined only when a real optimizing compiler builds tcc (gcc/clang host
 * cross).  tcc itself performs NO cross-call inlining of these: with the
 * definitions visible in the TU it still emits one local out-of-line copy per
 * TU and keeps every call (measured: 136 call-site relocations either way in
 * source/ir/codegen.c).  On the self-hosted armv8m device compiler that is
 * +112 KB of .text across the build for zero speedup, which is why these were
 * de-inlined in the first place -- see the note in tccir_operand.c.  Gating on
 * __TINYC__ keeps the device build on the shared copies and gives the host
 * cross the inlining (measured -6..-7% on body-heavy -O0 compiles). */
#ifndef __TINYC__
#define TCC_IROP_INLINE_ACCESSORS 1
#endif

/* Forward declaration: defined below after all helpers it needs.  Omitted when
 * inlined -- a static inline definition cannot follow a non-static
 * declaration. */
#ifndef TCC_IROP_INLINE_ACCESSORS
int32_t irop_get_vreg(const IROperand op);
#endif

/* Check if operand encodes a negative vreg (sentinel pattern).
 * Excludes IROP_NONE (vr == -1) which also matches the sentinel bit pattern. */
int irop_is_neg_vreg(const IROperand op);

/* Check if operand has no associated vreg */
int irop_has_no_vreg(const IROperand op);

/* Extract tag from operand (using bitfield) */
int irop_get_tag(const IROperand op);

/* Extract btype from operand (using bitfield) */
int irop_get_btype(const IROperand op);

/* A value SELECT lowers to one ITE block moving a SINGLE core register
 * (tcc_gen_machine_select_mop); 64-bit values live in register pairs and FP
 * values in VFP registers, neither of which that lowering moves, so
 * if-conversion must keep the branchy diamond for them.  The high word of an
 * INT64 select otherwise silently keeps whatever the register pair held. */
int irop_btype_select_lowerable(int btype);

/* Check if operand has a 64-bit type */
int irop_is_64bit(const IROperand op);

/* Check if operand needs a register pair (64-bit or complex) */
int irop_needs_pair(const IROperand op);

/* Check if operand has an immediate value */
int irop_is_immediate(const IROperand op);

/* Check if operand is a plain immediate (not a symref or lvalue).
 * Useful for passes that need a pure constant without symbol resolution. */
int irop_is_plain_imm(const IROperand op);

/* Get 64-bit integer value from operand (works for IMM32, I64, and STACKOFF)
 * Requires ir state for pool lookup. Pass NULL to only handle inline values. */
int64_t irop_get_imm64_ex(const struct TCCIRState *ir, IROperand op);

/* Get symbol from SYMREF operand. Requires ir state for pool lookup. */
struct Sym *irop_get_sym_ex(const struct TCCIRState *ir, IROperand op);

/* Get symref pool entry (includes symbol, addend, and flags) */
IRPoolSymref *irop_get_symref_ex(const struct TCCIRState *ir, IROperand op);

/* Convenience macros that use tcc_state->ir (requires tcc.h to be included first) */
#ifdef TCC_STATE_VAR
#define irop_get_imm64(op) irop_get_imm64_ex(TCC_STATE_VAR(ir), op)
#define irop_get_sym(op) irop_get_sym_ex(TCC_STATE_VAR(ir), op)
#define irop_get_symref(op) irop_get_symref_ex(TCC_STATE_VAR(ir), op)
#endif

/* Extract clean vreg value (type + position, for IR passes).
 * Body must stay identical to the out-of-line copy in tccir_operand.c. */
#ifdef TCC_IROP_INLINE_ACCESSORS
static inline int32_t irop_get_vreg(const IROperand op)
{
  /* IROP_NONE (vr == -1, all bits set) must return -1 before the negative vreg
   * sentinel check, because its bit pattern also matches the sentinel. */
  if (op.vr == -1)
    return -1;
  if (op.vreg_type == 0xF && (op.position & IROP_NEG_VREG_SENTINEL) == IROP_NEG_VREG_SENTINEL)
  {
    int neg_idx = op.position & 0xF;
    return -(neg_idx + 1);
  }
  if (op.vreg_type == 0)
    return -1;
  return (op.vreg_type << 28) | op.position;
}
#else
int32_t irop_get_vreg(const IROperand op);
#endif

/* Sentinel for "no operand" */
#define IROP_NONE                                                                                                      \
  ((IROperand){.vr = -1, .u = {.imm32 = 0}, .is_unsigned = 0, .is_static = 0, .is_sym = 0, .is_param = 0, .aux = 0})

/* Helper to initialize type-flag byte to defaults */
void irop_init_phys_regs(IROperand *op);

/* Helper to set vreg fields from a vreg value.
 * For negative vregs (temp locals like -1, -2, etc.), we use a special encoding:
 * - Set vreg_type to 0xF and position bits 4-17 to all 1s as sentinel
 * - Store (-vreg - 1) in position bits 0-3 (supports -1 to -16)
 * For positive vregs, encode normally in position and vreg_type bitfields.
 */
void irop_set_vreg(IROperand *op, int32_t vreg);

/* Encoding helpers */
IROperand irop_make_none(void);

IROperand irop_make_vreg(int32_t vreg, int btype);

IROperand irop_make_imm32(int32_t vreg, int32_t val, int btype);

IROperand irop_make_stackoff(int32_t vreg, int32_t offset, int is_lval, int is_llocal, int is_param_flag, int btype);

IROperand irop_make_f32(int32_t vreg, uint32_t bits);

IROperand irop_make_i64(int32_t vreg, uint32_t pool_idx, int btype);

IROperand irop_make_f64(int32_t vreg, uint32_t pool_idx);

IROperand irop_make_symref(int32_t vreg, uint32_t pool_idx, int is_lval, int is_local, int is_const, int btype);

/* Decoding helpers */
int irop_is_none(const IROperand op);

int irop_has_vreg(const IROperand op);

/* Get stack offset from STACKOFF operand (handles STRUCT split encoding) */
int32_t irop_get_stack_offset(const IROperand op);

/* Re-type an operand to a scalar (non-STRUCT) base type, keeping the payload
 * in the field the new btype reads it from.
 *
 * A STRUCT-typed operand uses the split `u.s` encoding: the CType pool index
 * in the low half and the tag's real payload in the HIGH half (u.s.aux_data) --
 * a STACKOFF's slot offset, a SYMREF's/I64's pool index, an IMM32's value.
 * Every scalar btype reads that payload from the full-width `u` instead.  So a
 * bare `op.btype = IROP_BTYPE_INT32` on a struct slot silently reinterprets
 * offset O as (O << 16 | ctype_idx): mem_inline narrowing a `memcpy(s.field,
 * "...", 4)` destination turned StackLoc[-92] into StackLoc[-6029312] and the
 * frame allocator sized tcc_output_yaff's prologue to match (6 MiB `sub sp` ->
 * process stack overflow at the first call).  Narrow slot operands through
 * here instead. */
IROperand irop_retype_scalar(IROperand op, int btype);

/* Get immediate value (for IMM32 tag - NOT for STACKOFF with struct types!) */
int32_t irop_get_imm32(const IROperand op);

/* Get pool index (for I64, F64, SYMREF tags) */
uint32_t irop_get_pool_idx(const IROperand op);

/* Check if operand is an lvalue (needs dereference) - uses bitfield */
int irop_op_is_lval(const IROperand op);

/* Check if operand has VT_LOCAL semantics - uses bitfield */
int irop_op_is_local(const IROperand op);

/* Check if operand has VT_LLOCAL semantics (double indirection) - uses bitfield */
int irop_op_is_llocal(const IROperand op);

/* Check if operand is constant - uses bitfield */
int irop_op_is_const(const IROperand op);
