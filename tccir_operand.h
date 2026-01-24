#pragma once

#include <stdint.h>

struct Sym;
struct TCCIRState;
struct SValue;

/* ============================================================================
 * Vreg encoding
 * ============================================================================
 * Vreg encoding: type in top 4 bits, position in bottom 20 bits.
 * Bits 20-27 are reserved for IROperand tag+flags encoding.
 */

typedef enum TCCIR_VREG_TYPE
{
  TCCIR_VREG_TYPE_VAR = 1,
  TCCIR_VREG_TYPE_TEMP = 2,
  TCCIR_VREG_TYPE_PARAM = 3,
} TCCIR_VREG_TYPE;

#define TCCIR_VREG_POSITION_MASK 0xFFFFF /* 20 bits for position */
#define TCCIR_DECODE_VREG_POSITION(vr) ((vr) & TCCIR_VREG_POSITION_MASK)
#define TCCIR_DECODE_VREG_TYPE(vr) ((vr) >> 28)
#define TCCIR_ENCODE_VREG(type, position) (((type) << 28) | ((position) & TCCIR_VREG_POSITION_MASK))

/* ============================================================================
 * IROperand: Compact 8-byte operand representation (vs ~56 byte SValue)
 * ============================================================================
 * Always includes vreg field so optimization passes can access it directly.
 * Tag and flags are packed into reserved bits of the vr field.
 *
 * vr field layout (32 bits):
 *   Bits 0-19:  vreg position
 *   Bits 20-22: tag (3 bits)
 *   Bits 23-24: flags (2 bits)
 *   Bits 25-27: reserved
 *   Bits 28-31: vreg type (from TCCIR_ENCODE_VREG)
 *
 * Special case: vr == -1 (0xFFFFFFFF) means "no vreg associated".
 */

/* Bit positions for tag/flags/btype in vr field */
#define IROP_VR_TAG_SHIFT 20
#define IROP_VR_TAG_MASK (0x7 << IROP_VR_TAG_SHIFT) /* 3 bits: 20-22 */
#define IROP_VR_FLAGS_SHIFT 23
#define IROP_VR_FLAGS_MASK (0x3 << IROP_VR_FLAGS_SHIFT) /* 2 bits: 23-24 */
#define IROP_VR_BTYPE_SHIFT 25
#define IROP_VR_BTYPE_MASK (0x7 << IROP_VR_BTYPE_SHIFT) /* 3 bits: 25-27 */
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
#define IROP_FLAG_LVAL (1u << 0)   /* value is an lvalue (needs dereference) */
#define IROP_FLAG_LLOCAL (1u << 1) /* VT_LLOCAL semantics (double indirection) */

/* Compressed basic type (stored in bits 25-27 of vr)
 * This allows reconstruction of type.t during iroperand_to_svalue().
 * Smaller integer types (byte/short) are promoted to INT32 for codegen. */
#define IROP_BTYPE_INT32 0   /* VT_VOID, VT_BYTE, VT_SHORT, VT_INT, VT_PTR, VT_BOOL */
#define IROP_BTYPE_INT64 1   /* VT_LLONG */
#define IROP_BTYPE_FLOAT32 2 /* VT_FLOAT */
#define IROP_BTYPE_FLOAT64 3 /* VT_DOUBLE, VT_LDOUBLE */
#define IROP_BTYPE_STRUCT 4  /* VT_STRUCT */
#define IROP_BTYPE_FUNC 5    /* VT_FUNC */
/* 6-7 reserved */

typedef struct __attribute__((packed)) IROperand
{
  int32_t vr; /* vreg id with embedded tag+flags+btype, -1 if not associated */
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

/* Extract btype from vr field (handles vr == -1 case) */
static inline int irop_get_btype(int32_t vr)
{
  if (vr < 0)
    return IROP_BTYPE_INT32; /* default */
  return (vr & IROP_VR_BTYPE_MASK) >> IROP_VR_BTYPE_SHIFT;
}

/* Extract clean vreg value (strips tag+flags+btype, preserves type bits) */
static inline int32_t irop_get_vreg(int32_t vr)
{
  if (vr < 0)
    return -1;
  /* Keep type bits (28-31) and position bits (0-19), clear tag/flags/btype (20-27) */
  int32_t result = (vr & 0xF00FFFFF);
  /* Position 0xFFFFF with type 0 is sentinel for "no vreg" */
  if ((result & IROP_VR_POSITION_MASK) == IROP_VR_POSITION_MASK && (result >> 28) == 0)
    return -1;
  return result;
}

/* Encode vreg with tag, flags, and btype */
static inline int32_t irop_encode_vr(int32_t vreg, int tag, int flags, int btype)
{
  if (vreg < 0)
  {
    /* For vr == -1, encode tag/flags/btype but use position 0xFFFFF (max) as sentinel.
     * This allows recovery of tag/flags/btype even without a valid vreg. */
    return IROP_VR_POSITION_MASK | (tag << IROP_VR_TAG_SHIFT) | (flags << IROP_VR_FLAGS_SHIFT) |
           (btype << IROP_VR_BTYPE_SHIFT);
  }
  /* Clear existing tag/flags/btype bits, then set new ones */
  int32_t clean = vreg & ~(IROP_VR_TAG_MASK | IROP_VR_FLAGS_MASK | IROP_VR_BTYPE_MASK);
  return clean | (tag << IROP_VR_TAG_SHIFT) | (flags << IROP_VR_FLAGS_SHIFT) | (btype << IROP_VR_BTYPE_SHIFT);
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

static inline IROperand irop_make_vreg(int32_t vreg, int btype)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_VREG, 0, btype);
  op.u.imm32 = 0;
  return op;
}

static inline IROperand irop_make_imm32(int32_t vreg, int32_t val, int btype)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_IMM32, 0, btype);
  op.u.imm32 = val;
  return op;
}

static inline IROperand irop_make_stackoff(int32_t vreg, int32_t offset, uint8_t flags, int btype)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_STACKOFF, flags, btype);
  op.u.imm32 = offset;
  return op;
}

static inline IROperand irop_make_f32(int32_t vreg, uint32_t bits)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_F32, 0, IROP_BTYPE_FLOAT32);
  op.u.f32_bits = bits;
  return op;
}

static inline IROperand irop_make_i64(int32_t vreg, uint32_t pool_idx)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_I64, 0, IROP_BTYPE_INT64);
  op.u.pool_idx = pool_idx;
  return op;
}

static inline IROperand irop_make_f64(int32_t vreg, uint32_t pool_idx)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_F64, 0, IROP_BTYPE_FLOAT64);
  op.u.pool_idx = pool_idx;
  return op;
}

static inline IROperand irop_make_symref(int32_t vreg, uint32_t pool_idx, uint8_t flags, int btype)
{
  IROperand op;
  op.vr = irop_encode_vr(vreg, IROP_TAG_SYMREF, flags, btype);
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

/* Get immediate value (for IMM32 or STACKOFF tags) */
static inline int32_t irop_get_imm32(IROperand op)
{
  return op.u.imm32;
}

/* Get pool index (for I64, F64, SYMREF tags) */
static inline uint32_t irop_get_pool_idx(IROperand op)
{
  return op.u.pool_idx;
}

/* Check if operand is an lvalue (needs dereference) */
static inline int irop_is_lval(IROperand op)
{
  return (irop_get_flags(op.vr) & IROP_FLAG_LVAL) != 0;
}

/* Check if operand has VT_LOCAL semantics (tag is STACKOFF or SYMREF with LOCAL flag) */
static inline int irop_is_local(IROperand op)
{
  int tag = irop_get_tag(op.vr);
  return (tag == IROP_TAG_STACKOFF);
}

/* Check if operand has VT_LLOCAL semantics (double indirection) */
static inline int irop_is_llocal(IROperand op)
{
  return (irop_get_flags(op.vr) & IROP_FLAG_LLOCAL) != 0;
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

/* IROperand pool management - separate pools for cache efficiency */
void tcc_ir_pools_init(struct TCCIRState *ir);
void tcc_ir_pools_free(struct TCCIRState *ir);
uint32_t tcc_ir_pool_add_i64(struct TCCIRState *ir, int64_t val);
uint32_t tcc_ir_pool_add_f64(struct TCCIRState *ir, uint64_t bits);
uint32_t tcc_ir_pool_add_symref(struct TCCIRState *ir, struct Sym *sym, int32_t addend, uint32_t flags);

/* IROperand <-> SValue conversion functions */
IROperand svalue_to_iroperand(struct TCCIRState *ir, const struct SValue *sv);
void iroperand_to_svalue(const struct TCCIRState *ir, IROperand op, struct SValue *out);
