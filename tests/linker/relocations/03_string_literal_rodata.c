/* A pointer initialized from a string literal must relocate into .rodata
 * against the literal's own local symbol (R_ARM_ABS32), not against the
 * variable itself. */
const char *msg = "hello world";
