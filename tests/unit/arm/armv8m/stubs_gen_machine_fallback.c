/*
 *  stubs_gen_machine_fallback.c - arm-thumb-gen.c fallbacks for the main UT
 *  binary only
 *
 *  tcc_gen_machine_number_of_registers()/tcc_get_abi_softcall_name() are
 *  real, non-static functions defined in arm-thumb-gen.c. The main
 *  run_unit_tests binary doesn't link arm-thumb-gen.c (see
 *  UT_COVERAGE_ONLY_SRCS in the Makefile), so it needs fakes here. The
 *  backend/ binary (build_backend/run_unit_tests_backend) links the REAL
 *  arm-thumb-gen.c instead -- this file must NOT be part of that binary's
 *  sources, or both would define these two symbols (multiple definition).
 */

#include <stddef.h>

/* From arm-thumb-gen.c — allocator init/shutdown. */
int tcc_gen_machine_number_of_registers(void)
{
  return 16;
}

/* From arm-thumb-gen.c — soft-float helper names; unit tests don't lower calls. */
struct SValue;

const char *tcc_get_abi_softcall_name(struct SValue *src1, struct SValue *src2,
                                       struct SValue *dest, int op)
{
  (void)src1; (void)src2; (void)dest; (void)op;
  return NULL;
}
