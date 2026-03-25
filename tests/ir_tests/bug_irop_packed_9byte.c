/* Test packed 9-byte struct bitfield operations under QEMU.
 * Replicates the exact patterns used by IROperand in the compiler.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define IROP_POSITION_NONE 0x1FFFF
#define IROP_NEG_VREG_SENTINEL 0x1FFF0
#define IROP_TAG_NONE   0
#define IROP_TAG_VREG   1
#define IROP_TAG_IMM32  2
#define TCCIR_VREG_POSITION_MASK 0x1FFFF

typedef struct __attribute__((packed)) IROperand {
  union {
    int32_t vr;
    struct {
      uint32_t position : 17;
      uint32_t is_complex : 1;
      uint32_t tag : 3;
      uint32_t is_lval : 1;
      uint32_t is_llocal : 1;
      uint32_t is_local : 1;
      uint32_t is_const : 1;
      uint32_t btype : 3;
      uint32_t vreg_type : 4;
    };
  };
  union {
    int32_t imm32;
    uint32_t f32_bits;
    uint32_t pool_idx;
  } u;
  uint8_t is_unsigned : 1;
  uint8_t is_static : 1;
  uint8_t is_sym : 1;
  uint8_t is_param : 1;
  uint8_t _pad : 4;
} IROperand;

/* Pool to hold IROperand entries - like iroperand_pool in the compiler */
static IROperand pool[20];
static int pool_count = 0;

static void irop_set_vreg(IROperand *op, int32_t vreg)
{
  if (vreg < 0) {
    int neg_idx = (int)(-vreg - 1);
    if (neg_idx > 15) neg_idx = 15;
    op->position = IROP_NEG_VREG_SENTINEL | (neg_idx & 0xF);
    op->vreg_type = 0xF;
  } else {
    op->position = vreg & TCCIR_VREG_POSITION_MASK;
    op->vreg_type = (vreg >> 28) & 0xF;
  }
}

static IROperand irop_make_imm32(int32_t vreg, int32_t val, int btype)
{
  IROperand op;
  op.vr = 0;
  irop_set_vreg(&op, vreg);
  op.tag = IROP_TAG_IMM32;
  op.is_lval = 0;
  op.is_llocal = 0;
  op.is_local = 0;
  op.is_const = 1;
  op.btype = btype;
  op.u.imm32 = val;
  op.is_unsigned = 0;
  op.is_static = 0;
  op.is_sym = 0;
  op.is_param = 0;
  op._pad = 0;
  return op;
}

static IROperand irop_make_none(void)
{
  IROperand op;
  op.vr = -1;
  op.u.imm32 = 0;
  op.is_unsigned = 0;
  op.is_static = 0;
  op.is_sym = 0;
  op.is_param = 0;
  op._pad = 0;
  return op;
}

static int irop_get_tag(const IROperand op)
{
  if (op.vr == -1) return IROP_TAG_NONE;
  if (op.position == IROP_POSITION_NONE && op.vreg_type == 0) return IROP_TAG_NONE;
  return op.tag;
}

static int irop_is_none(const IROperand op)
{
  return (op.position == IROP_POSITION_NONE && op.vreg_type == 0) || irop_get_tag(op) == IROP_TAG_NONE;
}

static int64_t irop_get_imm64_ex(IROperand op)
{
  int tag = irop_get_tag(op);
  if (tag == IROP_TAG_IMM32) return (int64_t)op.u.imm32;
  return 0;
}

#define TCCIR_ENCODE_PARAM(call_id, param_idx) (((uint32_t)(call_id) << 16) | ((uint32_t)(param_idx) & 0xFFFF))
#define TCCIR_DECODE_CALL_ID(encoded) (((uint32_t)(encoded)) >> 16)
#define TCCIR_DECODE_PARAM_IDX(encoded) ((int)((uint32_t)(encoded) & 0xFFFF))

/* Add an operand to the pool (simulates tcc_ir_put) */
static int pool_add(IROperand op)
{
  int idx = pool_count++;
  pool[idx] = op;
  return idx;
}

/* Read from pool (simulates tcc_ir_get_src2) */
static IROperand pool_get(int idx)
{
  return pool[idx];
}

int main(void)
{
  int errors = 0;

  printf("sizeof(IROperand)=%d\n", (int)sizeof(IROperand));

  /* Simulate the IR emission for strlen(str):
   * FUNCPARAMVAL src2 = irop_make_imm32(-1, ENCODE_PARAM(0,0), 0) [call_id=0, param_idx=0]
   * FUNCCALL src2 = irop_make_imm32(-1, ENCODE_CALL(0,1), 0) [call_id=0, argc=1]
   */
  int funcparam_idx = pool_add(irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_PARAM(0, 0), 0));
  int funccall_idx = pool_add(irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_PARAM(0, 1), 0));

  /* Add more entries to test at different offsets (unaligned) */
  int param1_idx = pool_add(irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_PARAM(1, 0), 0));
  int call1_idx = pool_add(irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_PARAM(1, 1), 0));
  int param2_idx = pool_add(irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_PARAM(2, 0), 0));
  int none_idx = pool_add(irop_make_none());

  /* Test reading back from pool */
  printf("\n--- Pool element addresses (9-byte stride) ---\n");
  for (int i = 0; i < pool_count; i++) {
    printf("pool[%d] addr offset: %d (mod4=%d)\n",
           i, (int)((char*)&pool[i] - (char*)&pool[0]),
           (int)(((char*)&pool[i] - (char*)&pool[0]) % 4));
  }

  /* Test 1: Read back FUNCPARAMVAL src2 (call_id=0) */
  {
    IROperand op = pool_get(funcparam_idx);
    int tag = irop_get_tag(op);
    int none = irop_is_none(op);
    int64_t imm = irop_get_imm64_ex(op);
    int call_id = (int)TCCIR_DECODE_CALL_ID((uint32_t)imm);
    int param_idx = TCCIR_DECODE_PARAM_IDX((uint32_t)imm);

    printf("\nTest 1: pool[%d] FUNCPARAMVAL (call_id=0, param=0)\n", funcparam_idx);
    printf("  vr=0x%08x tag=%d is_none=%d imm=%lld call_id=%d param=%d\n",
           (unsigned)op.vr, tag, none, (long long)imm, call_id, param_idx);

    /* Callsite scanner code: */
    int scanner_call_id = !irop_is_none(op) ? (int)TCCIR_DECODE_CALL_ID((uint32_t)op.u.imm32) : -1;
    printf("  scanner_call_id=%d (expect 0)\n", scanner_call_id);

    if (tag != IROP_TAG_IMM32) { printf("  FAIL tag=%d\n", tag); errors++; }
    if (none != 0) { printf("  FAIL is_none=%d\n", none); errors++; }
    if (scanner_call_id != 0) { printf("  FAIL scanner_call_id=%d\n", scanner_call_id); errors++; }
  }

  /* Test 2: Read from unaligned offset (pool[2]) */
  {
    IROperand op = pool_get(param1_idx);
    int tag = irop_get_tag(op);
    int none = irop_is_none(op);
    int scanner_call_id = !none ? (int)TCCIR_DECODE_CALL_ID((uint32_t)op.u.imm32) : -1;

    printf("\nTest 2: pool[%d] at offset %d (call_id=1)\n",
           param1_idx, (int)((char*)&pool[param1_idx] - (char*)&pool[0]));
    printf("  vr=0x%08x tag=%d is_none=%d scanner_call_id=%d (expect 1)\n",
           (unsigned)op.vr, tag, none, scanner_call_id);

    if (tag != IROP_TAG_IMM32) { printf("  FAIL tag\n"); errors++; }
    if (scanner_call_id != 1) { printf("  FAIL scanner_call_id\n"); errors++; }
  }

  /* Test 3: Read from another unaligned offset (pool[4]) */
  {
    IROperand op = pool_get(param2_idx);
    int tag = irop_get_tag(op);
    int none = irop_is_none(op);
    int scanner_call_id = !none ? (int)TCCIR_DECODE_CALL_ID((uint32_t)op.u.imm32) : -1;

    printf("\nTest 3: pool[%d] at offset %d (call_id=2)\n",
           param2_idx, (int)((char*)&pool[param2_idx] - (char*)&pool[0]));
    printf("  vr=0x%08x tag=%d is_none=%d scanner_call_id=%d (expect 2)\n",
           (unsigned)op.vr, tag, none, scanner_call_id);

    if (tag != IROP_TAG_IMM32) { printf("  FAIL tag\n"); errors++; }
    if (scanner_call_id != 2) { printf("  FAIL scanner_call_id\n"); errors++; }
  }

  /* Test 4: None entry */
  {
    IROperand op = pool_get(none_idx);
    int tag = irop_get_tag(op);
    int none = irop_is_none(op);

    printf("\nTest 4: pool[%d] NONE\n", none_idx);
    printf("  vr=0x%08x tag=%d is_none=%d (expect tag=0, none=1)\n",
           (unsigned)op.vr, tag, none);

    if (tag != IROP_TAG_NONE) { printf("  FAIL tag\n"); errors++; }
    if (none != 1) { printf("  FAIL is_none\n"); errors++; }
  }

  /* Test 5: NOP-out simulation (as in string_builtin_optimized)
   * Scan pool entries, find ones matching call_id=0, mark them. */
  {
    int nop_count = 0;
    int target_call_id = 0;
    printf("\nTest 5: NOP-out scan for call_id=%d\n", target_call_id);
    for (int i = 0; i < pool_count; i++) {
      IROperand src2 = pool_get(i);
      int encoded_call_id = (int)TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(src2));
      printf("  pool[%d]: irop_get_imm64_ex=%lld encoded_call_id=%d",
             i, (long long)irop_get_imm64_ex(src2), encoded_call_id);
      if (encoded_call_id == target_call_id && !irop_is_none(src2)) {
        printf(" -> MATCH (would NOP)");
        nop_count++;
      }
      printf("\n");
    }
    printf("  nop_count=%d (expect 2)\n", nop_count);
    if (nop_count != 2) { printf("  FAIL\n"); errors++; }
  }

  printf("\n%s (%d errors)\n", errors ? "FAILURES DETECTED" : "ALL TESTS PASSED", errors);
  return errors ? 1 : 0;
}
