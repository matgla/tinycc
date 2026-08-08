/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* int.c -- Integer constant folding and integer operator lowering.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* normalize values */
uint64_t value64(uint64_t l1, int t)
{
  uint64_t result;
  /* Complex integer types pack both real and imaginary parts into 64 bits
   * (e.g. _Complex int: real in low 32, imag in high 32).  Preserve the
   * full 64-bit packed representation regardless of the base type. */
  if ((t & VT_COMPLEX) || (t & VT_BTYPE) == VT_LLONG || (PTR_SIZE == 8 && (t & VT_BTYPE) == VT_PTR))
    result = l1;
  else if (t & VT_UNSIGNED)
    result = (uint32_t)l1;
  else
    result = (uint32_t)l1 | -(l1 & 0x80000000);
  return result;
}

static uint64_t gen_opic_sdiv(uint64_t a, uint64_t b)
{
  uint64_t x = (a >> 63 ? -a : a) / (b >> 63 ? -b : b);
  return (a ^ b) >> 63 ? -x : x;
}

static int gen_opic_lt(uint64_t a, uint64_t b)
{
  return (a ^ (uint64_t)1 << 63) < (b ^ (uint64_t)1 << 63);
}

/* handle integer constant optimizations and various machine
   independent opt */
void gen_opic(int op)
{
  SValue *v1 = vtop - 1;
  SValue *v2 = vtop;
  int t1 = v1->type.t & VT_BTYPE;
  int t2 = v2->type.t & VT_BTYPE;
  int c1 = (v1->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  int c2 = (v2->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  uint64_t l1 = c1 ? value64(v1->c.i, v1->type.t) : 0;
  uint64_t l2 = c2 ? value64(v2->c.i, v2->type.t) : 0;
  int shm = (t1 == VT_LLONG) ? 63 : 31;
  int r;

  /* Complex integer constant folding: operate component-wise */
  if (c1 && c2 && ((v1->type.t | v2->type.t) & VT_COMPLEX))
  {
    /* Both should be the same complex type at this point (after gen_cast_s) */
    int bt = t1; /* base type (e.g., VT_INT) */
    int shift = btype_size(bt) * 8;
    uint64_t mask = (bt == VT_LLONG) ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << shift) - 1);
    int64_t real1 = (int64_t)(l1 & mask);
    int64_t imag1 = (int64_t)((l1 >> shift) & mask);
    int64_t real2 = (int64_t)(l2 & mask);
    int64_t imag2 = (int64_t)((l2 >> shift) & mask);
    int64_t rr, ri;

    switch (op)
    {
    case '+':
      rr = real1 + real2;
      ri = imag1 + imag2;
      break;
    case '-':
      rr = real1 - real2;
      ri = imag1 - imag2;
      break;
    case '*':
      rr = real1 * real2 - imag1 * imag2;
      ri = real1 * imag2 + imag1 * real2;
      break;
    case TOK_EQ:
      v1->c.i = (real1 == real2) && (imag1 == imag2);
      v1->r |= v2->r & VT_NONCONST;
      vtop--;
      return;
    case TOK_NE:
      v1->c.i = (real1 != real2) || (imag1 != imag2);
      v1->r |= v2->r & VT_NONCONST;
      vtop--;
      return;
    default:
      goto general_case;
    }
    v1->c.i = ((uint64_t)(rr & mask)) | (((uint64_t)(ri & mask)) << shift);
    v1->r |= v2->r & VT_NONCONST;
    vtop--;
    return;
  }

  if (c1 && c2)
  {
    switch (op)
    {
    case '+':
      l1 += l2;
      break;
    case '-':
      l1 -= l2;
      break;
    case '&':
      l1 &= l2;
      break;
    case '^':
      l1 ^= l2;
      break;
    case '|':
      l1 |= l2;
      break;
    case '*':
      l1 *= l2;
      break;

    case TOK_PDIV:
    case '/':
    case '%':
    case TOK_UDIV:
    case TOK_UMOD:
      /* if division by zero, generate explicit division */
      if (l2 == 0)
      {
        if (CONST_WANTED && !NOEVAL_WANTED)
          tcc_error("division by zero in constant");
        goto general_case;
      }
      switch (op)
      {
      default:
        l1 = gen_opic_sdiv(l1, l2);
        break;
      case '%':
        l1 = l1 - l2 * gen_opic_sdiv(l1, l2);
        break;
      case TOK_UDIV:
        l1 = l1 / l2;
        break;
      case TOK_UMOD:
        l1 = l1 % l2;
        break;
      }
      break;
    case TOK_SHL:
      l1 <<= (l2 & shm);
      break;
    case TOK_SHR:
      l1 >>= (l2 & shm);
      break;
    case TOK_SAR:
      l1 = (l1 >> 63) ? ~(~l1 >> (l2 & shm)) : l1 >> (l2 & shm);
      break;
      /* tests */
    case TOK_ULT:
      l1 = l1 < l2;
      break;
    case TOK_UGE:
      l1 = l1 >= l2;
      break;
    case TOK_EQ:
      l1 = l1 == l2;
      break;
    case TOK_NE:
      l1 = l1 != l2;
      break;
    case TOK_ULE:
      l1 = l1 <= l2;
      break;
    case TOK_UGT:
      l1 = l1 > l2;
      break;
    case TOK_LT:
      l1 = gen_opic_lt(l1, l2);
      break;
    case TOK_GE:
      l1 = !gen_opic_lt(l1, l2);
      break;
    case TOK_LE:
      l1 = !gen_opic_lt(l2, l1);
      break;
    case TOK_GT:
      l1 = gen_opic_lt(l2, l1);
      break;
      /* logical */
    case TOK_LAND:
      l1 = l1 && l2;
      break;
    case TOK_LOR:
      l1 = l1 || l2;
      break;
    default:
      goto general_case;
    }
    v1->c.i = value64(l1, v1->type.t);
    v1->r |= v2->r & VT_NONCONST;
    vtop--;
    print_vstack("gen_opic(0)");
  }
  else
  {
    /* if commutative ops, put c2 as constant */
    if (c1 && (op == '+' || op == '&' || op == '^' || op == '|' || op == '*' || op == TOK_EQ || op == TOK_NE))
    {
      vswap();
      c2 = c1; // c = c1, c1 = c2, c2 = c;
      l2 = l1; // l = l1, l1 = l2, l2 = l;
    }
    /* Relational compares are not commutative, but they ARE reversible:
     * `K < x` is exactly `x > K`.  With the constant on the left the backend
     * must materialize it into a register first (`movs r1,#5; cmp r1,r0`);
     * on the right it encodes straight into the compare (`cmp r0,#5`).
     * Reverse the predicate and swap, so constant-on-the-left comparisons cost
     * what the hand-written form costs.  Reversal is exact — unlike negation it
     * involves no assumption about totality — so it is equally valid for the
     * unsigned forms.  (Only reached when at most one side is constant; the
     * both-constant case was folded above.) */
    else if (c1)
    {
      int rev_op = 0;
      switch (op)
      {
      case TOK_LT:  rev_op = TOK_GT;  break;
      case TOK_GT:  rev_op = TOK_LT;  break;
      case TOK_LE:  rev_op = TOK_GE;  break;
      case TOK_GE:  rev_op = TOK_LE;  break;
      case TOK_ULT: rev_op = TOK_UGT; break;
      case TOK_UGT: rev_op = TOK_ULT; break;
      case TOK_ULE: rev_op = TOK_UGE; break;
      case TOK_UGE: rev_op = TOK_ULE; break;
      default: break;
      }
      if (rev_op)
      {
        op = rev_op;
        vswap();
        c2 = c1;
        l2 = l1;
        c1 = 0; /* the left operand is now the non-constant one */
        l1 = 0;
      }
    }
    if (c1 && ((l1 == 0 && (op == TOK_SHL || op == TOK_SHR || op == TOK_SAR)) || (l1 == -1 && op == TOK_SAR)))
    {
      /* treat (0 << x), (0 >> x) and (-1 >> x) as constant */
      vpop();
      vtop->r |= VT_NONCONST;
    }
    else if (c2 && ((l2 == 0 && (op == '&' || op == '*')) ||
                    (op == '|' && (l2 == -1 || (l2 == 0xFFFFFFFF && t2 != VT_LLONG))) ||
                    (l2 == 1 && (op == '%' || op == TOK_UMOD))))
    {
      /* treat (x & 0), (x * 0), (x | -1) and (x % 1) as constant */
      if (l2 == 1)
        vtop->c.i = 0;
      vswap();
      vtop--;
      vtop->r |= VT_NONCONST;
      print_vstack("gen_opic(1)");
    }
    else if (c2 &&
             (((op == '*' || op == '/' || op == TOK_UDIV || op == TOK_PDIV) && l2 == 1) ||
              ((op == '+' || op == '-' || op == '|' || op == '^' || op == TOK_SHL || op == TOK_SHR || op == TOK_SAR) &&
               l2 == 0) ||
              (op == '&' && (l2 == -1 || (l2 == 0xFFFFFFFF && t2 != VT_LLONG)))))
    {
      /* filter out NOP operations like x*1, x-0, x&-1... */
      vtop--;
      print_vstack("gen_opic(2)");
    }
    else if (c2 && (op == '*' || op == TOK_PDIV || op == TOK_UDIV))
    {
      /* Try to use shifts instead of muls or divs.  The power-of-2 test is
       * delegated to is_power_of_2() (a standalone function) rather than an
       * inline `(l2 & (l2 - 1)) == 0`: the armv8m self-host cross miscompiles
       * that 64-bit AND/compare *in this function's register context*, judging
       * non-powers-of-2 (e.g. 10) to be powers of 2 and rewriting `x * 10` to
       * `x << 3` (= x * 8).  is_power_of_2() compiles correctly as its own TU
       * symbol, sidestepping the context-specific miscompile.  (Multipliers
       * with bit 63 set fall through as a plain MUL — correct, just unoptimised.)
       */
      int shn = is_power_of_2((int64_t)l2);
      if (shn >= 0)
      {
        vtop->c.i = shn;
        if (op == '*')
          op = TOK_SHL;
        else if (op == TOK_PDIV)
          op = TOK_SAR;
        else
          op = TOK_SHR;
      }
      goto general_case;
    }
    else if (c2 && (op == '+' || op == '-') &&
             (r = vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM),
              r == (VT_CONST | VT_SYM) || r == VT_LOCAL ||
                  (nocode_wanted && r == (VT_LOCAL | VT_LVAL) && (vtop[-1].type.t & VT_BTYPE) == VT_PTR)))
    {
      /* symbol + constant case */
      if (op == '-')
        l2 = -l2;
      l2 += vtop[-1].c.i;
      /* The backends can't always deal with addends to symbols
         larger than +-1<<31.  Don't construct such.  */
      if ((int)l2 != l2)
        goto general_case;
      vtop--;
      print_vstack("gen_opic(3)");
      if (nocode_wanted && r == (VT_LOCAL | VT_LVAL))
        vtop->r &= ~VT_LVAL;
      vtop->c.i = l2;
    }
    else if (op == '-' && CONST_WANTED && (v1->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_SYM) &&
             (v2->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_SYM))
    {
      /* Label difference in constant context: &&lab1 - &&lab0.
         Record the two symbols for deferred resolution after codegen,
         produce a pure VT_CONST result with the addend difference. */
      pending_label_diff_plus = v1->sym;
      pending_label_diff_minus = v2->sym;
      v1->c.i = v1->c.i - v2->c.i;
      v1->r = VT_CONST;
      v1->sym = NULL;
      vtop--;
    }
    else
    {
    general_case:
      /* call low level op generator */
      if (t1 == VT_LLONG || t2 == VT_LLONG || (PTR_SIZE == 8 && (t1 == VT_PTR || t2 == VT_PTR)))
        gen_opl(op);
      else
      {
        // gen_opi(op);
        tcc_ir_gen_i(tcc_state->ir, op);
      }
    }
    if (vtop->r == VT_CONST)
      vtop->r |= VT_NONCONST; /* is const, but only by optimization */
  }
}
