/* A data object that stores the address of a function is a data-to-code
 * reference: it must use an absolute relocation (R_ARM_ABS32), not a
 * PC-relative branch relocation like calls use. */
int add(int a, int b) {
  return a + b;
}

int (*fp)(int, int) = add;
