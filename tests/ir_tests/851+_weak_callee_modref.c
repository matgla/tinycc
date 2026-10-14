struct S { int a, b, c, d, e; };
extern struct S GS;
void helper(void) { GS.e++; }
