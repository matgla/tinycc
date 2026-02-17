/*
 * Reproducer for native TCC codegen bug: union field read after store.
 *
 * The IROperand type uses a union { int32_t imm32; uint16_t pool_idx; ... }
 * When TCCIR_ENCODE_PARAM stores a value via c.i (int64_t) and it's read
 * back via u.imm32, the cross-compiled TCC may generate incorrect code
 * for the union member access.
 */
#include <stdio.h>
#include <stdint.h>

/* Minimal reproduction of IROperand union layout */
typedef struct {
    uint8_t tag;
    uint8_t btype;
    union {
        int32_t imm32;
        uint16_t pool_idx;
        struct {
            uint16_t aux_data;
            int16_t vreg;
        } s;
        uint32_t f32_bits;
    } u;
} MiniOperand;

/* Reproduce the TCCIR_ENCODE/DECODE macros */
#define ENCODE_PARAM(call_id, param_idx) \
    ((int64_t)(int32_t)(((uint32_t)(call_id) << 16) | ((uint32_t)(param_idx) & 0xFFFFu)))
#define DECODE_CALL_ID(encoded) ((int)(((uint32_t)(encoded)) >> 16))
#define DECODE_PARAM_IDX(encoded) ((int)(((uint32_t)(encoded)) & 0xFFFF))

/* Simulate how tccgen.c stores the param encoding */
static void store_param(MiniOperand *op, int call_id, int param_idx)
{
    op->tag = 2;  /* IROP_TAG_IMM32 */
    op->btype = 0;
    /* This is how tccgen.c stores it: through c.i (int64_t) cast down */
    int64_t encoded = ENCODE_PARAM(call_id, param_idx);
    op->u.imm32 = (int32_t)encoded;
}

/* Simulate how arm-thumb-callsite.c reads it back */
static int read_call_id(MiniOperand *op)
{
    return DECODE_CALL_ID((uint32_t)op->u.imm32);
}

static int read_param_idx(MiniOperand *op)
{
    return DECODE_PARAM_IDX((uint32_t)op->u.imm32);
}

int main(void)
{
    MiniOperand ops[8];
    int i;
    int errors = 0;

    /* Store call_id=0..5, param_idx=0 (single-arg calls like printf) */
    for (i = 0; i < 6; i++) {
        store_param(&ops[i], i, 0);
    }
    /* Also test multi-arg: call_id=4, param_idx=1 */
    store_param(&ops[6], 4, 1);
    /* And call_id=5, param_idx=1 */
    store_param(&ops[7], 5, 1);

    /* Read back and verify */
    for (i = 0; i < 6; i++) {
        int cid = read_call_id(&ops[i]);
        int pid = read_param_idx(&ops[i]);
        printf("op[%d]: call_id=%d param_idx=%d (raw=0x%08x)\n",
               i, cid, pid, (unsigned)ops[i].u.imm32);
        if (cid != i || pid != 0) {
            printf("  ERROR: expected call_id=%d param_idx=0\n", i);
            errors++;
        }
    }
    /* Check multi-arg entries */
    {
        int cid = read_call_id(&ops[6]);
        int pid = read_param_idx(&ops[6]);
        printf("op[6]: call_id=%d param_idx=%d (raw=0x%08x)\n",
               cid, pid, (unsigned)ops[6].u.imm32);
        if (cid != 4 || pid != 1) {
            printf("  ERROR: expected call_id=4 param_idx=1\n");
            errors++;
        }
    }
    {
        int cid = read_call_id(&ops[7]);
        int pid = read_param_idx(&ops[7]);
        printf("op[7]: call_id=%d param_idx=%d (raw=0x%08x)\n",
               cid, pid, (unsigned)ops[7].u.imm32);
        if (cid != 5 || pid != 1) {
            printf("  ERROR: expected call_id=5 param_idx=1\n");
            errors++;
        }
    }

    if (errors == 0)
        printf("PASS: all union reads correct\n");
    else
        printf("FAIL: %d errors\n", errors);

    return errors;
}
