/* Phase 4: DIV/IMOD lowering and runtime-helper selection. */

int div_signed(int a, int b) { return a / b; }
unsigned div_unsigned(unsigned a, unsigned b) { return a / b; }
int mod_signed(int a, int b) { return a % b; }
unsigned mod_unsigned(unsigned a, unsigned b) { return a % b; }
