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
#define IROP_TAG_STACKOFF 3 /* payload.imm32: signed 32-bit FP-relative offset */
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
      uint32_t vreg_type : 4;  /* TCCIR_VREG_TYPE_* (28-31) */
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
  uint8_t _pad : 4;        /* unused — available for future flags */
} IROperand;

_Static_assert(sizeof(IROperand) == 9, "IROperand must be 9 bytes");

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

/* Get CType for struct operands (returns NULL for non-struct types) */
struct CType *irop_get_ctype(IROperand op);

/* Debug: compare SValue with IROperand and print differences (returns 1 if mismatch) */
int irop_compare_svalue(const struct TCCIRState *ir, const struct SValue *sv, IROperand op, const char *context);

/* Position sentinel value: max 17-bit value means "no position" */
#define IROP_POSITION_NONE 0x1FFFF

/* Check if operand encodes a negative vreg (sentinel pattern).
 * Excludes IROP_NONE (vr == -1) which also matches the sentinel bit pattern. */
static inline int irop_is_neg_vreg(const IROperand op)
{
  if (op.vr == -1)
    return 0; /* IROP_NONE, not a negative vreg */
  return op.vreg_type == 0xF && (op.position & 0x1FFF0) == IROP_NEG_VREG_SENTINEL;
}

/* Check if operand has no associated vreg */
static inline int irop_has_no_vreg(const IROperand op)
{
  /* Either negative vreg sentinel OR the old vr < 0 check for IROP_NONE */
  return irop_is_neg_vreg(op) || (op.position == IROP_POSITION_NONE && op.vreg_type == 0);
}

/* Extract tag from operand (using bitfield) */
static inline int irop_get_tag(const IROperand op)
{
  /* IROP_NONE has vr == -1 (all bits set), return TAG_NONE for it */
  if (op.vr == -1)
    return IROP_TAG_NONE;
  /* For negative vregs (encoded with sentinel), tag is still valid in bitfield */
  if (op.position == IROP_POSITION_NONE && op.vreg_type == 0)
    return IROP_TAG_NONE;
  return op.tag;
}

/* Extract btype from operand (using bitfield) */
static inline int irop_get_btype(const IROperand op)
{
  if (op.vr == -1)
    return IROP_BTYPE_INT32; /* IROP_NONE default */
  if (op.position == IROP_POSITION_NONE && op.vreg_type == 0)
    return IROP_BTYPE_INT32; /* default */
  return op.btype;
}

/* Check if operand has a 64-bit type */
static inline int irop_is_64bit(const IROperand op)
{
  int btype = irop_get_btype(op);
  return btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT64;
}

/* Check if operand needs a register pair (64-bit or complex) */
static inline int irop_needs_pair(const IROperand op)
{
  if (op.is_complex)
    return 1;
  int btype = irop_get_btype(op);
  return btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT64;
}

/* Check if operand has an immediate value */
static inline int irop_is_immediate(const IROperand op)
{
  int tag = irop_get_tag(op);
  return tag == IROP_TAG_IMM32 || tag == IROP_TAG_F32 || tag == IROP_TAG_I64 || tag == IROP_TAG_F64;
}

/* Get 64-bit integer value from operand (works for IMM32, I64, and STACKOFF)
 * Requires ir state for pool lookup. Pass NULL to only handle inline values. */
static inline int64_t irop_get_imm64_ex(const struct TCCIRState *ir, IROperand op)
{
  int tag = irop_get_tag(op);
  switch (tag)
  {
  case IROP_TAG_IMM32:
    /* Sign-extend 32-bit immediate to 64-bit */
    return (int64_t)op.u.imm32;
  case IROP_TAG_STACKOFF:
    /* For STRUCT types, offset is stored directly in aux_data; otherwise in imm32 */
    if (op.btype == IROP_BTYPE_STRUCT)
      return (int64_t)((int32_t)op.u.s.aux_data);
    return (int64_t)op.u.imm32;
  case IROP_TAG_I64:
    /* Look up in pool */
    if (ir)
    {
      int64_t *p = tcc_ir_pool_get_i64_ptr(ir, op.u.pool_idx);
      if (p)
        return *p;
    }
    return 0;
  case IROP_TAG_F32:
    /* Treat float bits as unsigned 32-bit */
    return (int64_t)(uint32_t)op.u.f32_bits;
  case IROP_TAG_F64:
    /* Look up in pool and return raw bits */
    if (ir)
    {
      uint64_t *p = tcc_ir_pool_get_f64_ptr(ir, op.u.pool_idx);
      if (p)
        return (int64_t)*p;
    }
    return 0;
  default:
    return 0;
  }
}

/* Get symbol from SYMREF operand. Requires ir state for pool lookup. */
static inline struct Sym *irop_get_sym_ex(const struct TCCIRState *ir, IROperand op)
{
  if (irop_get_tag(op) != IROP_TAG_SYMREF)
    return NULL;
  if (!ir)
    return NULL;
  /* For STRUCT types, symref index is in aux_data */
  uint32_t idx = (op.btype == IROP_BTYPE_STRUCT) ? (uint32_t)(uint16_t)op.u.s.aux_data : op.u.pool_idx;
  IRPoolSymref *entry = tcc_ir_pool_get_symref_ptr(ir, idx);
  return entry ? entry->sym : NULL;
}

/* Get symref pool entry (includes symbol, addend, and flags) */
static inline IRPoolSymref *irop_get_symref_ex(const struct TCCIRState *ir, IROperand op)
{
  if (irop_get_tag(op) != IROP_TAG_SYMREF)
    return NULL;
  if (!ir)
    return NULL;
  /* For STRUCT types, symref index is in aux_data */
  uint32_t idx = (op.btype == IROP_BTYPE_STRUCT) ? (uint32_t)(uint16_t)op.u.s.aux_data : op.u.pool_idx;
  return tcc_ir_pool_get_symref_ptr(ir, idx);
}

/* Convenience macros that use tcc_state->ir (requires tcc.h to be included first) */
#ifdef TCC_STATE_VAR
#define irop_get_imm64(op) irop_get_imm64_ex(TCC_STATE_VAR(ir), op)
#define irop_get_sym(op) irop_get_sym_ex(TCC_STATE_VAR(ir), op)
#define irop_get_symref(op) irop_get_symref_ex(TCC_STATE_VAR(ir), op)
#endif

/* Extract clean vreg value (type + position, for IR passes) */
static inline int32_t irop_get_vreg(const IROperand op)
{
  /* IROP_NONE (vr == -1, all bits set) must return -1 before the negative vreg
   * sentinel check, because its bit pattern also matches the sentinel. */
  if (op.vr == -1)
    return -1;
  /* Check for negative vreg sentinel: vreg_type=0xF and position bits match sentinel */
  if (op.vreg_type == 0xF && (op.position & IROP_NEG_VREG_SENTINEL) == IROP_NEG_VREG_SENTINEL)
  {
    /* Decode negative vreg: idx 0 -> -1, idx 1 -> -2, etc.
     * Matches irop_set_vreg which encodes: neg_idx = (-vreg) - 1 */
    int neg_idx = op.position & 0xF;
    return -(neg_idx + 1);
  }
  /* Position == max sentinel with vreg_type 0 means no vreg (-1) */
  if (op.position == IROP_POSITION_NONE && op.vreg_type == 0)
    return -1;
  /* Reconstruct vreg: type in bits 28-31, position in bits 0-16 */
  return (op.vreg_type << 28) | op.position;
}

/* Sentinel for "no operand" */
#define IROP_NONE                                                                                                      \
  ((IROperand){.vr = -1, .u = {.imm32 = 0}, .is_unsigned = 0, .is_static = 0, .is_sym = 0, .is_param = 0, ._pad = 0})

/* Helper to initialize type-flag byte to defaults */
static inline void irop_init_phys_regs(IROperand *op)
{
  op->is_unsigned = 0;
  op->is_static = 0;
  op->is_sym = 0;
  op->is_param = 0;
  op->_pad = 0;
}

/* Helper to set vreg fields from a vreg value.
 * For negative vregs (temp locals like -1, -2, etc.), we use a special encoding:
 * - Set vreg_type to 0xF and position bits 4-17 to all 1s as sentinel
 * - Store (-vreg - 1) in position bits 0-3 (supports -1 to -16)
 * For positive vregs, encode normally in position and vreg_type bitfields.
 */
static inline void irop_set_vreg(IROperand *op, int32_t vreg)
{
  if (vreg < 0)
  {
    /* Encode small negative: -1 -> idx 0, -2 -> idx 1, etc. */
    int neg_idx = (int)(-vreg - 1);
    if (neg_idx > 15)
      neg_idx = 15; /* Clamp to 4 bits */
    /* Sentinel in upper bits, neg index in lower 4 bits */
    op->position = IROP_NEG_VREG_SENTINEL | (neg_idx & 0xF);
    op->vreg_type = 0xF;
  }
  else
  {
    op->position = vreg & TCCIR_VREG_POSITION_MASK;
    op->vreg_type = (vreg >> 28) & 0xF;
  }
}

/* Encoding helpers */
static inline IROperand irop_make_none(void)
{
  IROperand op;
  op.vr = -1;
  op.u.imm32 = 0;
  irop_init_phys_regs(&op);
  return op;
}

static inline IROperand irop_make_vreg(int32_t vreg, int btype)
{
  IROperand op;
  op.vr = 0; /* clear all bits first */
  irop_set_vreg(&op, vreg);
  op.tag = IROP_TAG_VREG;
  op.is_lval = 0;
  op.is_llocal = 0;
  op.is_local = 0;
  op.is_const = 0;
  op.btype = btype;
  op.u.imm32 = 0;
  irop_init_phys_regs(&op);
  return op;
}

static inline IROperand irop_make_imm32(int32_t vreg, int32_t val, int btype)
{
  IROperand op;
  op.vr = 0;
  irop_set_vreg(&op, vreg);
  op.tag = IROP_TAG_IMM32;
  op.is_lval = 0;
  op.is_llocal = 0;
  op.is_local = 0;
  op.is_const = 1; /* immediates are constants */
  op.btype = btype;
  op.u.imm32 = val;
  irop_init_phys_regs(&op);
  return op;
}

static inline IROperand irop_make_stackoff(int32_t vreg, int32_t offset, int is_lval, int is_llocal, int is_param_flag,
                                           int btype)
{
  IROperand op;
  op.vr = 0;
  irop_set_vreg(&op, vreg);
  op.tag = IROP_TAG_STACKOFF;
  op.is_lval = is_lval;
  op.is_llocal = is_llocal;
  op.is_local = 1; /* stack offsets are local */
  op.is_const = 0;
  op.btype = btype;
  op.u.imm32 = offset;
  irop_init_phys_regs(&op);
  op.is_param = is_param_flag; /* Set AFTER irop_init_phys_regs to avoid being overwritten */
  return op;
}

static inline IROperand irop_make_f32(int32_t vreg, uint32_t bits)
{
  IROperand op;
  op.vr = 0;
  irop_set_vreg(&op, vreg);
  op.tag = IROP_TAG_F32;
  op.is_lval = 0;
  op.is_llocal = 0;
  op.is_local = 0;
  op.is_const = 1;
  op.btype = IROP_BTYPE_FLOAT32;
  op.u.f32_bits = bits;
  irop_init_phys_regs(&op);
  return op;
}

static inline IROperand irop_make_i64(int32_t vreg, uint32_t pool_idx, int btype)
{
  IROperand op;
  op.vr = 0;
  irop_set_vreg(&op, vreg);
  op.tag = IROP_TAG_I64;
  op.is_lval = 0;
  op.is_llocal = 0;
  op.is_local = 0;
  op.is_const = 1;
  op.btype = btype;
  op.u.pool_idx = pool_idx;
  irop_init_phys_regs(&op);
  return op;
}

static inline IROperand irop_make_f64(int32_t vreg, uint32_t pool_idx)
{
  IROperand op;
  op.vr = 0;
  irop_set_vreg(&op, vreg);
  op.tag = IROP_TAG_F64;
  op.is_lval = 0;
  op.is_llocal = 0;
  op.is_local = 0;
  op.is_const = 1;
  op.btype = IROP_BTYPE_FLOAT64;
  op.u.pool_idx = pool_idx;
  irop_init_phys_regs(&op);
  return op;
}

static inline IROperand irop_make_symref(int32_t vreg, uint32_t pool_idx, int is_lval, int is_local, int is_const,
                                         int btype)
{
  IROperand op;
  op.vr = 0;
  irop_set_vreg(&op, vreg);
  op.tag = IROP_TAG_SYMREF;
  op.is_lval = is_lval;
  op.is_llocal = 0;
  op.is_local = is_local;
  op.is_const = is_const;
  op.btype = btype;
  op.u.pool_idx = pool_idx;
  irop_init_phys_regs(&op);
  op.is_sym = 1; /* symbol reference */
  return op;
}

/* Decoding helpers */
static inline int irop_is_none(const IROperand op)
{
  /* Check for IROP_NONE: position=max, vreg_type=0, or tag=NONE */
  return (op.position == IROP_POSITION_NONE && op.vreg_type == 0) || irop_get_tag(op) == IROP_TAG_NONE;
}

static inline int irop_has_vreg(const IROperand op)
{
  /* Has vreg if not IROP_NONE and not the negative vreg sentinel returning -1 specifically for "no vreg" */
  int vreg = irop_get_vreg(op);
  return vreg >= 0 || (vreg < -1); /* -2, -3, etc. are temp locals - they DO have a vreg */
}

/* Get stack offset from STACKOFF operand (handles STRUCT split encoding) */
static inline int32_t irop_get_stack_offset(const IROperand op)
{
  if (op.btype == IROP_BTYPE_STRUCT)
    return (int32_t)op.u.s.aux_data; /* Stored directly */
  return op.u.imm32;
}

/* Get immediate value (for IMM32 tag - NOT for STACKOFF with struct types!) */
static inline int32_t irop_get_imm32(const IROperand op)
{
  return op.u.imm32;
}

/* Get pool index (for I64, F64, SYMREF tags) */
static inline uint32_t irop_get_pool_idx(const IROperand op)
{
  return op.u.pool_idx;
}

/* Check if operand is an lvalue (needs dereference) - uses bitfield */
static inline int irop_op_is_lval(const IROperand op)
{
  if (op.vr < 0)
    return 0;
  return op.is_lval;
}

/* Check if operand has VT_LOCAL semantics - uses bitfield */
static inline int irop_op_is_local(const IROperand op)
{
  if (op.vr < 0)
    return 0;
  return op.is_local;
}

/* Check if operand has VT_LLOCAL semantics (double indirection) - uses bitfield */
static inline int irop_op_is_llocal(const IROperand op)
{
  if (op.vr < 0)
    return 0;
  return op.is_llocal;
}

/* Check if operand is constant - uses bitfield */
static inline int irop_op_is_const(const IROperand op)
{
  if (op.vr < 0)
    return 0;
  return op.is_const;
}
