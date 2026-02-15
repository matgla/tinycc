/**
 * Regression test for const char *const global pointer access under PIC
 * with text/data separation (-mpic-data-is-text-relative).
 *
 * Bug: When a global const pointer (const char *const) is accessed through
 * the GOT with R_ARM_GOT32, the codegen generates a double dereference:
 *   1. Load GOT slot → address of pointer variable
 *   2. Load pointer value from variable → string address
 * But the pointer value stored in the variable is a link-time address that
 * requires a data relocation (R_ARM_ABS32) to be patched at load time.
 * If the data relocation is not applied, the loaded pointer is stale.
 *
 * This test must be compiled with: -mpic-data-is-text-relative
 * to enable text_and_data_separation mode.
 */
#include <stdio.h>

/* Multi-string constant using NUL-separated values (same pattern as
   target_machine_defs in arm-thumb-gen.c) */
const char *const machine_defs = "__test_def1__\0"
                                 "__test_def2__\0"
                                 "__test_def3__\0";

static void putdef(const char *p)
{
  printf("def: %s\n", p);
}

/* Walk NUL-separated string list (same as putdefs in tccpp.c) */
static void putdefs(const char *p)
{
  while (*p)
  {
    putdef(p);
    p += strlen(p) + 1;
  }
}

int main(void)
{
  putdefs(machine_defs);
  printf("ok\n");
  return 0;
}
