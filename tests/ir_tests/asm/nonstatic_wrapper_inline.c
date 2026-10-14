/* kernel-exported-trivial-wrappers-out-of-line: a non-static one-call
 * wrapper whose token body exceeds the registration threshold (100 tokens
 * at -O2) can only be promoted by gen_function's post-optimization wrapper
 * tier -- which used to require VT_STATIC, so every exported C-convention
 * wrapper stayed a real call at its intra-TU sites.
 *
 * The dead d<i> locals pad the token stream past the threshold without
 * adding IR ops (uniform `y += k*0` padding re-rolls into a loop, which
 * ir_body_is_acyclic_wrapper rightly rejects), and the eight global loads
 * put the body past the 8-slot trivial tier into the wrapper tier proper
 * (10 ops, exactly one call, acyclic).
 *
 * Assertions (test_codegen_asm.py::test_nonstatic_one_call_wrapper_inlines):
 * the intra-TU caller branches straight to the wrapped callee (no call to
 * big_wrap) while the GLOBAL big_wrap definition is still emitted for
 * other translation units. */

void inner(int);
extern int g1, g2, g3, g4, g5, g6, g7, g8;

void big_wrap(int x)
{
  int d1 = x ^ 1; (void)d1;
  int d2 = x ^ 2; (void)d2;
  int d3 = x ^ 3; (void)d3;
  int d4 = x ^ 4; (void)d4;
  int d5 = x ^ 5; (void)d5;
  int d6 = x ^ 6; (void)d6;
  int d7 = x ^ 7; (void)d7;
  int d8 = x ^ 8; (void)d8;
  int d9 = x ^ 9; (void)d9;
  int d10 = x ^ 10; (void)d10;
  int d11 = x ^ 11; (void)d11;
  int d12 = x ^ 12; (void)d12;
  int d13 = x ^ 13; (void)d13;
  int d14 = x ^ 14; (void)d14;
  int d15 = x ^ 15; (void)d15;
  int d16 = x ^ 16; (void)d16;
  int d17 = x ^ 17; (void)d17;
  int d18 = x ^ 18; (void)d18;
  int d19 = x ^ 19; (void)d19;
  int d20 = x ^ 20; (void)d20;
  int d21 = x ^ 21; (void)d21;
  int d22 = x ^ 22; (void)d22;
  int d23 = x ^ 23; (void)d23;
  int d24 = x ^ 24; (void)d24;
  int d25 = x ^ 25; (void)d25;
  int d26 = x ^ 26; (void)d26;
  int d27 = x ^ 27; (void)d27;
  int d28 = x ^ 28; (void)d28;
  int d29 = x ^ 29; (void)d29;
  int d30 = x ^ 30; (void)d30;
  int d31 = x ^ 31; (void)d31;
  int d32 = x ^ 32; (void)d32;
  int d33 = x ^ 33; (void)d33;
  int d34 = x ^ 34; (void)d34;
  int d35 = x ^ 35; (void)d35;
  int d36 = x ^ 36; (void)d36;
  int d37 = x ^ 37; (void)d37;
  int d38 = x ^ 38; (void)d38;
  int d39 = x ^ 39; (void)d39;
  int d40 = x ^ 40; (void)d40;
  int d41 = x ^ 41; (void)d41;
  int d42 = x ^ 42; (void)d42;
  int d43 = x ^ 43; (void)d43;
  int d44 = x ^ 44; (void)d44;
  inner(x + g1 + g2 + g3 + g4 + g5 + g6 + g7 + g8);
}

void caller(int n)
{
  big_wrap(n);
}
