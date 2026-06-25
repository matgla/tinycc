/* Test post-increment of struct member on ARM.
 * This replicates the pattern: call_id = tcc_state->ir->next_call_id++
 */
#include <stdio.h>

typedef struct Inner {
  int next_call_id;
  int other_field;
} Inner;

typedef struct Outer {
  Inner *ir;
  int dummy;
} Outer;

int main(void)
{
  Inner ir_data = {0, 42};
  Outer state = {&ir_data, 99};
  Outer *tcc_state = &state;

  int errors = 0;

  /* Test 1: Simple post-increment of struct->member */
  int id0 = tcc_state->ir->next_call_id++;
  printf("id0=%d next=%d (expect 0, 1)\n", id0, tcc_state->ir->next_call_id);
  if (id0 != 0) { printf("  FAIL\n"); errors++; }
  if (tcc_state->ir->next_call_id != 1) { printf("  FAIL next\n"); errors++; }

  int id1 = tcc_state->ir->next_call_id++;
  printf("id1=%d next=%d (expect 1, 2)\n", id1, tcc_state->ir->next_call_id);
  if (id1 != 1) { printf("  FAIL\n"); errors++; }
  if (tcc_state->ir->next_call_id != 2) { printf("  FAIL next\n"); errors++; }

  int id2 = tcc_state->ir->next_call_id++;
  printf("id2=%d next=%d (expect 2, 3)\n", id2, tcc_state->ir->next_call_id);
  if (id2 != 2) { printf("  FAIL\n"); errors++; }
  if (tcc_state->ir->next_call_id != 3) { printf("  FAIL next\n"); errors++; }

  /* Test 2: Post-increment with conditional (matches the actual code pattern) */
  ir_data.next_call_id = 0; /* reset */
  int nocode = 0;
  int call_id = 0;
  if (!nocode)
    call_id = tcc_state->ir->next_call_id++;
  printf("\ncond id=%d next=%d (expect 0, 1)\n", call_id, tcc_state->ir->next_call_id);
  if (call_id != 0) { printf("  FAIL\n"); errors++; }
  if (tcc_state->ir->next_call_id != 1) { printf("  FAIL next\n"); errors++; }

  /* Test 3: Multiple increments in sequence (as in gfunc_call) */
  ir_data.next_call_id = 0;
  int ids[5];
  for (int i = 0; i < 5; i++) {
    ids[i] = tcc_state->ir->next_call_id++;
  }
  printf("\nSequential: ");
  for (int i = 0; i < 5; i++) {
    printf("%d ", ids[i]);
    if (ids[i] != i) { printf("FAIL "); errors++; }
  }
  printf("(expect 0 1 2 3 4)\n");
  printf("next=%d (expect 5)\n", tcc_state->ir->next_call_id);
  if (tcc_state->ir->next_call_id != 5) { printf("  FAIL next\n"); errors++; }

  /* Test 4: Post-increment where result feeds into another struct member */
  ir_data.next_call_id = 0;
  {
    const int new_call_id = tcc_state->ir->next_call_id++;
    unsigned encoded = ((unsigned)(new_call_id) << 16) | (0 & 0xFFFF);
    printf("\nEncoded: new_call_id=%d encoded=0x%08x (expect 0, 0x00000000)\n",
           new_call_id, encoded);
    if (new_call_id != 0) { printf("  FAIL\n"); errors++; }
    if (encoded != 0) { printf("  FAIL encoded\n"); errors++; }
  }
  {
    const int new_call_id = tcc_state->ir->next_call_id++;
    unsigned encoded = ((unsigned)(new_call_id) << 16) | (0 & 0xFFFF);
    printf("Encoded: new_call_id=%d encoded=0x%08x (expect 1, 0x00010000)\n",
           new_call_id, encoded);
    if (new_call_id != 1) { printf("  FAIL\n"); errors++; }
    if (encoded != 0x00010000) { printf("  FAIL encoded\n"); errors++; }
  }

  printf("\n%s (%d errors)\n", errors ? "FAILURES DETECTED" : "ALL TESTS PASSED", errors);
  return errors ? 1 : 0;
}
