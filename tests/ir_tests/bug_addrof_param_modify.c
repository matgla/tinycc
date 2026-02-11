/*
 * Regression test: modify register-passed parameter via its address.
 *
 * Exercises the gaddrof() fix from a different angle: instead of the
 * strstart pattern, this test takes &param on both int and pointer
 * parameters, passes the address to helper functions that modify the
 * original value through the pointer.
 *
 * Before the fix, &val would produce the VALUE of val (e.g., 0 or 99)
 * instead of a pointer to val's stack slot, causing modify_via_ptr()
 * to write to an arbitrary address.
 */

#include <stdio.h>

void modify_int(int *p)
{
  *p = 42;
}

int test_addrof_int(int val)
{
  modify_int(&val);
  return val;
}

void modify_str(const char **pp, const char *new_val)
{
  *pp = new_val;
}

const char *test_addrof_str(const char *s)
{
  modify_str(&s, "replaced");
  return s;
}

/* Also test with a second parameter (not in R0) */
int test_addrof_second_param(int dummy, int val)
{
  modify_int(&val);
  return val + dummy;
}

int main(void)
{
  printf("int(0): %d\n", test_addrof_int(0));
  printf("int(99): %d\n", test_addrof_int(99));
  printf("str: %s\n", test_addrof_str("original"));
  printf("second(10,0): %d\n", test_addrof_second_param(10, 0));
  return 0;
}
