/*
 * Minimal reproducer: 10-byte packed struct with bitfield union + array indexing.
 * Stripped down from bug_packed10_array.c to pinpoint exact failing pattern.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct __attribute__((packed)) Op10
{
  union
  {
    int32_t vr;
    struct
    {
      uint32_t position : 18;
      uint32_t tag : 3;
      uint32_t flag1 : 1;
      uint32_t flag2 : 1;
      uint32_t flag3 : 1;
      uint32_t flag4 : 1;
      uint32_t btype : 3;
      uint32_t vtype : 4;
    };
  };
  int32_t payload;
  uint8_t r0;
  uint8_t r1;
} Op10;

_Static_assert(sizeof(Op10) == 10, "Op10 must be 10 bytes");

int main(void)
{
  Op10 pool[4];
  int errors = 0;
  memset(pool, 0, sizeof(pool));

  /* Store known tag values */
  pool[0].vr = 0;
  pool[0].tag = 1;
  pool[0].payload = 100;

  pool[1].vr = 0;
  pool[1].tag = 7;
  pool[1].payload = 200;

  pool[2].vr = 0;
  pool[2].tag = 2;
  pool[2].payload = 0x10000;

  pool[3].vr = 0;
  pool[3].tag = 5;
  pool[3].payload = 300;

  /* Read back via direct array indexing */
  for (int i = 0; i < 4; i++)
  {
    int t = pool[i].tag;
    int p = pool[i].payload;
    int expected_tag, expected_payload;
    switch (i)
    {
    case 0:
      expected_tag = 1;
      expected_payload = 100;
      break;
    case 1:
      expected_tag = 7;
      expected_payload = 200;
      break;
    case 2:
      expected_tag = 2;
      expected_payload = 0x10000;
      break;
    case 3:
      expected_tag = 5;
      expected_payload = 300;
      break;
    }
    if (t != expected_tag)
    {
      printf("FAIL pool[%d].tag: expected %d, got %d\n", i, expected_tag, t);
      errors++;
    }
    else
    {
      printf("OK pool[%d].tag = %d\n", i, t);
    }
    if (p != expected_payload)
    {
      printf("FAIL pool[%d].payload: expected %d, got %d\n", i, expected_payload, p);
      errors++;
    }
    else
    {
      printf("OK pool[%d].payload = %d\n", i, p);
    }
  }

  /* Also check raw vr word */
  printf("--- raw vr check ---\n");
  for (int i = 0; i < 4; i++)
  {
    uint32_t raw = (uint32_t)pool[i].vr;
    int tag_from_raw = (raw >> 18) & 7;
    int tag_from_field = pool[i].tag;
    printf("pool[%d]: vr=0x%08x tag_raw=%d tag_field=%d\n", i, (unsigned)raw, tag_from_raw, tag_from_field);
    if (tag_from_raw != tag_from_field)
    {
      printf("  MISMATCH between raw and bitfield read!\n");
      errors++;
    }
  }

  if (errors == 0)
    printf("ALL PASSED\n");
  else
    printf("FAILED: %d errors\n", errors);
  return errors;
}
