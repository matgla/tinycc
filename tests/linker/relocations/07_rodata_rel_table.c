/* A __rodata_relative table naming read-only data of another object: the
 * compiler cannot see where it lands, the linker resolves (and checks) it. */
extern const char rel_target[];

const char *__rodata_relative const rel_table[] = {"local", rel_target + 2, 0};

void entry(void)
{
}
