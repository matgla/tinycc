// A 64-bit equality CMP whose flag consumer a tail merge moved behind a jump.
//
// The codegen lowers a 64-bit CMP by the condition of the instruction that
// reads its flags: EQ/NE get `cmp hi; it eq; cmpeq lo`, anything else the
// relational `cmp lo; sbcs hi`, whose Z reflects the high word only.  Tail
// merging (cross_jump, cross_jump_alloc) turned `CMP; SELECT` into `CMP; JMP
// tail` with the SELECT in the shared tail, the consumer search stopped at the
// JMP, and the equality took the relational form: `l2 == smin` came out true
// for l2 = 0, 1, 0x7fffffff...  This is gen_opic's tautology fold, lifted
// verbatim; built into the native tcc it folded ordinary signed compares to
// constants and broke ~1,000 device tests (selfhost-const-compare-fold).
#include <stdio.h>
#include <stdint.h>
enum { TOK_ULT = 0x92, TOK_UGE, TOK_EQ, TOK_NE, TOK_ULE, TOK_UGT, TOK_LT = 0x9c, TOK_GE, TOK_LE, TOK_GT };
enum { VT_INT = 3, VT_LLONG = 4 };
/* gen_opic's tautology fold, lifted verbatim (its inputs as parameters) */
__attribute__((noinline)) int taut_of(int op, int c1, int c2, int t1, int t2, uint64_t l2)
{
  int taut = -1;
  if (c2 && !c1 && t1 == t2 && (t1 == VT_INT || t1 == VT_LLONG))
  {
    const int is64 = t1 == VT_LLONG;
    const uint64_t umax = is64 ? ~(uint64_t)0 : 0xffffffffull;
    const uint64_t smin = is64 ? (uint64_t)1 << 63 : 0xffffffff80000000ull;
    const uint64_t smax = is64 ? ~(uint64_t)0 >> 1 : 0x7fffffffull;
    switch (op)
    {
    case TOK_ULT: taut = l2 == 0 ? 0 : -1; break;
    case TOK_UGE: taut = l2 == 0 ? 1 : -1; break;
    case TOK_UGT: taut = l2 == umax ? 0 : -1; break;
    case TOK_ULE: taut = l2 == umax ? 1 : -1; break;
    case TOK_LT: taut = l2 == smin ? 0 : -1; break;
    case TOK_GE: taut = l2 == smin ? 1 : -1; break;
    case TOK_GT: taut = l2 == smax ? 0 : -1; break;
    case TOK_LE: taut = l2 == smax ? 1 : -1; break;
    default: break;
    }
  }
  return taut;
}
int main(void)
{
  static const int ops[] = {TOK_ULT, TOK_UGE, TOK_UGT, TOK_ULE, TOK_LT, TOK_GE, TOK_GT, TOK_LE, TOK_EQ};
  static const uint64_t vals[] = {0, 1, 5, 6, 0xffffffffull, 0x7fffffffull, 0xffffffff80000000ull,
                                  ~0ull, ~0ull >> 1, 1ull << 63, 0x100000000ull};
  unsigned h = 0;
  for (int t = VT_INT; t <= VT_LLONG; t++)
    for (unsigned o = 0; o < sizeof ops / sizeof *ops; o++)
      for (unsigned v = 0; v < sizeof vals / sizeof *vals; v++)
      {
        int r = taut_of(ops[o], 0, 1, t, t, vals[v]);
        h = h * 31 + (unsigned)(r + 1);
        if (r >= 0)
          printf("t=%d op=%#x l2=%#llx -> %d\n", t, ops[o], (unsigned long long)vals[v], r);
      }
  printf("hash=%08x\n", h);
  return 0;
}
