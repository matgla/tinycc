/*
 * Regression test: address-of operator on register-passed parameter.
 *
 * Bug: When taking the address of a function parameter that was passed
 * in a register (e.g., R0), gaddrof() failed to emit a LEA instruction.
 * It only cleared VT_LVAL, causing the generated code to load the VALUE
 * of the parameter instead of computing its ADDRESS on the stack.
 *
 * Symptom: strstart(&name, prefix) received the char* value of name
 * instead of a char** pointer to name's stack slot, causing a crash
 * when dereferencing.
 *
 * Fix: Added handling for VT_PARAM in gaddrof() (tccgen.c) to emit
 * TCCIR_OP_LEA with VT_LOCAL|VT_PARAM source, producing correct
 * stack address computation.
 */

#include <stdio.h>

/*
 * Simulates strstart(): receives char** (pointer to pointer),
 * checks if string starts with prefix, advances the pointer past it.
 */
int strstart(const char **strp, const char *prefix)
{
  const char *str = *strp;
  const char *p = prefix;
  while (*p)
  {
    if (*str != *p)
      return 0;
    str++;
    p++;
  }
  *strp = str; /* advance past prefix */
  return 1;
}

/*
 * The bug triggers here: 'name' is passed in R0 (register param).
 * Taking &name must produce a valid char** pointing to name's stack slot.
 */
int check_prefix(const char *name, const char *prefix)
{
  if (strstart(&name, prefix))
  {
    printf("match: rest='%s'\n", name);
    return 1;
  }
  printf("no match\n");
  return 0;
}

int main(void)
{
  check_prefix("hello_world", "hello_");
  check_prefix("foobar", "foo");
  check_prefix("test", "xyz");
  return 0;
}
