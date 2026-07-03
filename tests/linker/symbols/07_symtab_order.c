/* Standard ELF symtab convention: all STB_LOCAL symbols must precede the
 * first non-local (global/weak) symbol, and the section header's sh_info
 * field must equal the index of that first non-local symbol. Interleave
 * local and global data/function definitions to check tcc groups them
 * correctly rather than preserving source order. */
int g1 = 1;
static int s1 = 2;
int g2(void) {
  return 1;
}
static int s2(void) {
  return 2;
}
int g3 = 3;
static int s3 = 4;
