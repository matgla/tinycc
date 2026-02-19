/*
 * Reproducer: 10-byte packed struct array indexing + bitfield access.
 *
 * Tests the exact IROperand layout: 10-byte packed struct with bitfield
 * union in first 4 bytes, payload union in next 4 bytes, and 2 bytes
 * of packed bitfield flags. Array indexing with stride 10 (non-power-of-2)
 * combined with bitfield reads is a likely cross-TCC codegen failure point.
 *
 * The native TCC bug manifests as tag=7 (SYMREF) when tag=2 (IMM32) was
 * stored, suggesting either:
 *   - Array index * 10 multiplication is wrong
 *   - Bitfield extraction from the vr word is wrong
 *   - Struct return/copy of 10-byte packed struct is wrong
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAG_NONE 0
#define TAG_VREG 1
#define TAG_IMM32 2
#define TAG_STACKOFF 3
#define TAG_F32 4
#define TAG_I64 5
#define TAG_F64 6
#define TAG_SYMREF 7

typedef struct __attribute__((packed)) TestOperand
{
  /* First 4 bytes: union of raw int32 and bitfields */
  union
  {
    int32_t vr;
    struct
    {
      uint32_t position : 18;
      uint32_t tag : 3;
      uint32_t is_lval : 1;
      uint32_t is_llocal : 1;
      uint32_t is_local : 1;
      uint32_t is_const : 1;
      uint32_t btype : 3;
      uint32_t vreg_type : 4;
    };
  };
  /* Next 4 bytes: payload union */
  union
  {
    int32_t imm32;
    uint32_t f32_bits;
    uint32_t pool_idx;
    struct
    {
      uint16_t ctype_idx;
      int16_t aux_data;
    } s;
  } u;
  /* Last 2 bytes: packed flag bitfields */
  uint8_t pr0_reg : 5;
  uint8_t pr0_spilled : 1;
  uint8_t is_unsigned : 1;
  uint8_t is_static : 1;
  uint8_t pr1_reg : 5;
  uint8_t pr1_spilled : 1;
  uint8_t is_sym : 1;
  uint8_t is_param : 1;
} TestOperand;

/* Verify struct is 10 bytes */
_Static_assert(sizeof(TestOperand) == 10, "TestOperand must be 10 bytes");

/* Create an IMM32 operand - mirrors irop_make_imm32 */
static TestOperand make_imm32(int32_t val)
{
  TestOperand op;
  memset(&op, 0, sizeof(op));
  op.vr = 0;
  op.position = 0x3FFF0; /* -1 sentinel */
  op.tag = TAG_IMM32;
  op.is_const = 1;
  op.btype = 0;
  op.u.imm32 = val;
  op.pr0_reg = 31;
  op.pr1_reg = 31;
  return op;
}

/* Create a SYMREF operand - mirrors irop_make_symref */
static TestOperand make_symref(uint32_t pidx)
{
  TestOperand op;
  memset(&op, 0, sizeof(op));
  op.vr = 0;
  op.position = 5;
  op.tag = TAG_SYMREF;
  op.is_const = 1;
  op.btype = 0;
  op.u.pool_idx = pidx;
  op.pr0_reg = 31;
  op.pr1_reg = 31;
  return op;
}

/* Create a VREG operand */
static TestOperand make_vreg(int pos)
{
  TestOperand op;
  memset(&op, 0, sizeof(op));
  op.vr = 0;
  op.position = pos;
  op.tag = TAG_VREG;
  op.btype = 0;
  op.pr0_reg = 31;
  op.pr1_reg = 31;
  return op;
}

/* Simulate the pool access pattern: pool[base + off] then read fields.
 * This is what tcc_ir_get_src2 does. Mark noinline to prevent optimization. */
__attribute__((noinline)) static TestOperand pool_read(TestOperand *pool, int base, int off)
{
  return pool[base + off];
}

/* Read tag from operand returned by pool access.
 * This is the exact pattern that fails: return from 10-byte struct,
 * then read bitfield. */
__attribute__((noinline)) static int get_tag(TestOperand *pool, int base, int off)
{
  TestOperand op = pool[base + off];
  return (int)op.tag;
}

/* Read imm32 from returned operand */
__attribute__((noinline)) static int32_t get_imm32(TestOperand *pool, int base, int off)
{
  TestOperand op = pool[base + off];
  return op.u.imm32;
}

/* Encode/decode param - same macros as TCCIR_ENCODE_PARAM/DECODE */
#define ENCODE_PARAM(call_id, param_idx) ((int32_t)(((uint32_t)(call_id) << 16) | ((uint32_t)(param_idx) & 0xFFFFu)))
#define DECODE_CALL_ID(encoded) ((int)(((uint32_t)(encoded)) >> 16))
#define DECODE_PARAM_IDX(encoded) ((int)(((uint32_t)(encoded)) & 0xFFFF))

static int errors = 0;

static void check_int(const char *desc, int expected, int actual)
{
  if (expected != actual)
  {
    printf("FAIL: %s: expected %d, got %d\n", desc, expected, actual);
    errors++;
  }
  else
  {
    printf("OK: %s = %d\n", desc, actual);
  }
}

static void check_u32(const char *desc, uint32_t expected, uint32_t actual)
{
  if (expected != actual)
  {
    printf("FAIL: %s: expected 0x%x, got 0x%x\n", desc, (unsigned)expected, (unsigned)actual);
    errors++;
  }
  else
  {
    printf("OK: %s = 0x%x\n", desc, (unsigned)actual);
  }
}

int main(void)
{
  /* Test 1: Basic struct size and layout */
  printf("sizeof(TestOperand) = %d\n", (int)sizeof(TestOperand));

  /* Test 2: Allocate pool of 10-byte packed structs - exercises stride-10 indexing */
  TestOperand pool[8];
  memset(pool, 0xCC, sizeof(pool));

  /* Simulate a FUNCPARAMVAL instruction's operands:
   *   pool[0] = dest (VREG)
   *   pool[1] = src1 (SYMREF - the string arg like "Hello, world!")
   *   pool[2] = src2 (IMM32 - ENCODE_PARAM(1, 0) = 0x00010000)
   * Then a FUNCCALL:
   *   pool[3] = dest (VREG)
   *   pool[4] = src1 (SYMREF - function pointer)
   *   pool[5] = src2 (IMM32 - ENCODE_CALL(1, 1) = 0x00010001)
   */
  pool[0] = make_vreg(10);
  pool[1] = make_symref(42);
  pool[2] = make_imm32(ENCODE_PARAM(1, 0));
  pool[3] = make_vreg(11);
  pool[4] = make_symref(99);
  pool[5] = make_imm32(ENCODE_PARAM(1, 1));
  pool[6] = make_imm32(ENCODE_PARAM(2, 0));
  pool[7] = make_imm32(0x12345678);

  /* Test 3: Direct array access - tag discrimination */
  check_int("pool[0].tag", TAG_VREG, pool[0].tag);
  check_int("pool[1].tag", TAG_SYMREF, pool[1].tag);
  check_int("pool[2].tag", TAG_IMM32, pool[2].tag);
  check_int("pool[3].tag", TAG_VREG, pool[3].tag);
  check_int("pool[4].tag", TAG_SYMREF, pool[4].tag);
  check_int("pool[5].tag", TAG_IMM32, pool[5].tag);

  /* Test 4: Through function call (struct return) - THE critical pattern */
  check_int("get_tag(pool,0,0)", TAG_VREG, get_tag(pool, 0, 0));
  check_int("get_tag(pool,0,1)", TAG_SYMREF, get_tag(pool, 0, 1));
  check_int("get_tag(pool,0,2)", TAG_IMM32, get_tag(pool, 0, 2));
  check_int("get_tag(pool,3,0)", TAG_VREG, get_tag(pool, 3, 0));
  check_int("get_tag(pool,3,1)", TAG_SYMREF, get_tag(pool, 3, 1));
  check_int("get_tag(pool,3,2)", TAG_IMM32, get_tag(pool, 3, 2));

  /* Test 5: IMM32 payload decode - exercises the exact failing path */
  int32_t raw2 = get_imm32(pool, 0, 2);
  check_u32("pool[2].imm32 raw", (uint32_t)ENCODE_PARAM(1, 0), (uint32_t)raw2);
  check_int("pool[2] call_id", 1, DECODE_CALL_ID(raw2));
  check_int("pool[2] param_idx", 0, DECODE_PARAM_IDX(raw2));

  int32_t raw5 = get_imm32(pool, 3, 2);
  check_u32("pool[5].imm32 raw", (uint32_t)ENCODE_PARAM(1, 1), (uint32_t)raw5);
  check_int("pool[5] call_id", 1, DECODE_CALL_ID(raw5));
  check_int("pool[5] argc", 1, DECODE_PARAM_IDX(raw5));

  /* Test 6: Full struct return and field access */
  TestOperand got2 = pool_read(pool, 0, 2);
  check_int("returned[2].tag", TAG_IMM32, got2.tag);
  check_u32("returned[2].imm32", (uint32_t)ENCODE_PARAM(1, 0), (uint32_t)got2.u.imm32);
  check_int("returned[2].is_const", 1, got2.is_const);

  TestOperand got5 = pool_read(pool, 3, 2);
  check_int("returned[5].tag", TAG_IMM32, got5.tag);
  check_u32("returned[5].imm32", (uint32_t)ENCODE_PARAM(1, 1), (uint32_t)got5.u.imm32);

  /* Test 7: Position field (18-bit) preserved through store/load */
  check_u32("pool[1].position", 5, pool[1].position);
  check_u32("pool[0].position", 10, pool[0].position);

  /* Test 8: Walking backward through pool (like callsite scanner) */
  printf("--- backward scan simulation ---\n");
  int call_id_target = 1;
  int found = 0;
  for (int j = 5; j >= 0; j--)
  {
    TestOperand op = pool[j];
    if (op.tag == TAG_IMM32)
    {
      int cid = DECODE_CALL_ID(op.u.imm32);
      int pidx = DECODE_PARAM_IDX(op.u.imm32);
      printf("pool[%d]: tag=IMM32 call_id=%d param=%d\n", j, cid, pidx);
      if (cid == call_id_target)
      {
        found++;
      }
    }
    else
    {
      printf("pool[%d]: tag=%d\n", j, op.tag);
    }
  }
  check_int("found params for call_id=1", 2, found);

  /* Test 9: Verify the vr raw word for known tag values.
   * For tag=IMM32(2), bits 18-20 = 010 → vr & 0x001C0000 = 0x00080000
   * For tag=SYMREF(7), bits 18-20 = 111 → vr & 0x001C0000 = 0x001C0000 */
  uint32_t vr2 = (uint32_t)pool[2].vr;
  uint32_t tag_bits = (vr2 >> 18) & 7;
  check_int("pool[2] vr tag bits", TAG_IMM32, (int)tag_bits);

  uint32_t vr1 = (uint32_t)pool[1].vr;
  uint32_t tag_bits1 = (vr1 >> 18) & 7;
  check_int("pool[1] vr tag bits", TAG_SYMREF, (int)tag_bits1);

  if (errors == 0)
  {
    printf("ALL PASSED\n");
  }
  else
  {
    printf("FAILED: %d errors\n", errors);
  }
  return errors;
}
