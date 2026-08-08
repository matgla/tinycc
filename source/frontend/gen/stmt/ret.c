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

/* ret.c -- return statement lowering.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* ------------------------------------------------------------------------- */
/* return from function */
#ifndef TCC_TARGET_ARM64
void gfunc_return(CType *func_type)
{
  /* Complex types are composite (two elements) and must follow the same
   * return convention as structs — via hidden pointer or packed registers
   * depending on gfunc_sret().  Check VT_COMPLEX first since their VT_BTYPE
   * is VT_FLOAT/VT_DOUBLE, not VT_STRUCT. */
  if ((func_type->t & VT_BTYPE) == VT_STRUCT || (func_type->t & VT_COMPLEX))
  {
    CType type, ret_type;
    int ret_align, ret_nregs, regsize;
    ret_nregs = gfunc_sret(func_type, func_var, &ret_type, &ret_align, &regsize);
    if (ret_nregs < 0)
    {
#ifdef TCC_TARGET_RISCV64
      arch_transfer_ret_regs(0);
#endif
    }
    else if (0 == ret_nregs)
    {
      if (func_type->t & VT_COMPLEX)
      {
        /* Complex sret return: copy the complex value to the caller's
         * return buffer.
         *
         * If vtop is an lval (already in memory — e.g. a local variable),
         * we use its address directly.  This is critical for complex
         * types larger than 8 bytes (e.g. _Complex long long, _Complex
         * double) because TCCIR_OP_STORE only handles up to 64-bit values
         * and would silently truncate 16-byte complex types.
         *
         * If vtop is an rvalue in a register pair (e.g. result of complex
         * float arithmetic), we spill to a temp local first.
         *
         * Small word-aligned copies are inlined as word LOAD/STORE pairs
         * through sret_ptr; larger or unaligned sizes fall back to
         * memmove. */
        int complex_size, complex_align;
        complex_size = type_size(func_type, &complex_align);

        /* src_mem describes WHERE the source bytes live, as an lvalue
         * (VT_LVAL set).  Either points at vtop directly (in-memory
         * source) or at a fresh spill slot (rvalue source).
         *
         * src_is_opaque_rvalue tracks whether the spill came from an
         * rvalue produced by an opaque complex operation (e.g. complex
         * FMUL/FDIV, lowered to per-component math inside the backend
         * but represented as a single IR FMUL/FDIV op).  In that case,
         * the IR does NOT show explicit reads from the imaginary-half
         * parameter slots, so DCE may eliminate them — relying on the
         * memmove call to act as a memory-barrier.  We therefore keep
         * the memmove for the rvalue-spill path. */
        SValue src_mem;
        memset(&src_mem, 0, sizeof(src_mem));
        int src_is_opaque_rvalue = 0;

        if (vtop->r & VT_LVAL)
        {
          src_mem = *vtop;
        }
        else
        {
          /* Source is an rvalue (register pair) — spill to temp local.
           * This path handles _Complex float/int (8 bytes) which can fit
           * in a register pair and be stored via a single 64-bit STORE. */
          loc = (loc - complex_size) & -complex_align;
          int tmp_loc = loc;

          SValue tmp_dst;
          memset(&tmp_dst, 0, sizeof(tmp_dst));
          tmp_dst.type = vtop->type;
          tmp_dst.r = VT_LOCAL | VT_LVAL;
          tmp_dst.vr = -1;
          tmp_dst.c.i = tmp_loc;
          tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, vtop, NULL, &tmp_dst);

          src_mem.type = vtop->type;
          src_mem.r = VT_LOCAL | VT_LVAL;
          src_mem.vr = -1;
          src_mem.c.i = tmp_loc;
          src_is_opaque_rvalue = 1;
        }

        /* Load the sret pointer from func_vc.
         * func_vc is a stack slot holding the hidden sret pointer passed in r0. */
        SValue sret_slot;
        memset(&sret_slot, 0, sizeof(sret_slot));
        sret_slot.type.t = VT_PTR;
        sret_slot.r = VT_LOCAL | VT_LVAL;
        sret_slot.vr = -1;
        sret_slot.c.i = func_vc;

        SValue sret_ptr;
        memset(&sret_ptr, 0, sizeof(sret_ptr));
        sret_ptr.type.t = VT_PTR;
        sret_ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        sret_ptr.r = 0;

        tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &sret_slot, NULL, &sret_ptr);

        /* Inline word-by-word copy for small, word-aligned complex
         * returns — skips the memmove call, exposes stores to DCE/CSE,
         * and matches the small-struct optimization in vstore().
         *
         * Skip when the source is an opaque rvalue spill: removing the
         * memmove there can confuse DCE (see comment on
         * src_is_opaque_rvalue above). */
        if (!src_is_opaque_rvalue && complex_size <= 16 &&
            !(complex_size & 3) && !(complex_align & 3))
        {
          CType word_type;
          word_type.t = VT_INT;
          word_type.ref = NULL;

          /* c.i on a register-deref lvalue (e.g. `return *p;`) is not a
           * frame offset and is ignored by the codegen — compute src + off
           * explicitly via ADD, like the vstore() inline copy does. */
          int src_is_reg_deref = ((src_mem.r & VT_VALMASK) < VT_CONST);
          for (int off = 0; off < complex_size; off += 4)
          {
            /* Load word from src_mem + off */
            SValue src_word = src_mem;
            src_word.type = word_type;

            if (src_is_reg_deref && off != 0)
            {
              SValue off_imm;
              svalue_init(&off_imm);
              off_imm.type.t = VT_INT;
              off_imm.r = VT_CONST;
              off_imm.vr = -1;
              off_imm.c.i = off;

              SValue src_base;
              memset(&src_base, 0, sizeof(src_base));
              src_base.type.t = VT_PTR;
              src_base.vr = src_mem.vr;
              src_base.r = 0;

              SValue src_ptr;
              svalue_init(&src_ptr);
              src_ptr.type.t = VT_PTR;
              src_ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
              src_ptr.r = 0;

              tcc_ir_put(tcc_state->ir, TCCIR_OP_ADD, &src_base, &off_imm, &src_ptr);

              src_word.r = VT_LVAL;
              src_word.vr = src_ptr.vr;
              src_word.sym = NULL;
              src_word.c.i = 0;
            }
            else
            {
              src_word.c.i += off;
            }

            SValue tmp_word;
            svalue_init(&tmp_word);
            tmp_word.type = word_type;
            tmp_word.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
            tmp_word.r = 0;

            tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &src_word, NULL, &tmp_word);

            /* Resolve store target = sret_ptr + off (vreg holding addr) */
            SValue dst_ptr;
            if (off == 0)
            {
              dst_ptr = sret_ptr;
            }
            else
            {
              SValue off_imm;
              svalue_init(&off_imm);
              off_imm.type.t = VT_INT;
              off_imm.r = VT_CONST;
              off_imm.vr = -1;
              off_imm.c.i = off;

              svalue_init(&dst_ptr);
              dst_ptr.type.t = VT_PTR;
              dst_ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
              dst_ptr.r = 0;

              tcc_ir_put(tcc_state->ir, TCCIR_OP_ADD, &sret_ptr, &off_imm, &dst_ptr);
            }

            /* Store tmp_word through dst_ptr (vreg with VT_LVAL = deref) */
            SValue store_dst;
            svalue_init(&store_dst);
            store_dst.type = word_type;
            store_dst.r = VT_LVAL;
            store_dst.vr = dst_ptr.vr;

            tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &tmp_word, NULL, &store_dst);
          }
        }
        else
        {
          /* Fallback: memmove(sret_ptr, &src_mem, complex_size) */
          SValue src_addr;
          memset(&src_addr, 0, sizeof(src_addr));
          src_addr.type.t = VT_PTR;
          src_addr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
          src_addr.r = 0;

          SValue src_for_lea = src_mem;
          src_for_lea.type.t = VT_PTR;
          src_for_lea.r &= ~VT_LVAL; /* take address */
          tcc_ir_put(tcc_state->ir, TCCIR_OP_LEA, &src_for_lea, NULL, &src_addr);

          SValue size_sv;
          memset(&size_sv, 0, sizeof(size_sv));
          size_sv.type.t = VT_INT;
          size_sv.r = VT_CONST;
          size_sv.vr = -1;
          size_sv.c.i = complex_size;

          vpush_helper_func(
#ifdef TCC_ARM_EABI
              (!(complex_align & 3)) ? TOK_memmove4 : TOK_memmove
#else
              TOK_memmove
#endif
          );

          SValue param_num;
          const int call_id = tcc_state->ir->next_call_id++;
          svalue_init(&param_num);
          param_num.vr = -1;
          param_num.r = VT_CONST;

          param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
          tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &sret_ptr, &param_num, NULL);
          param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
          tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &src_addr, &param_num, NULL);
          param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 2);
          tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &size_sv, &param_num, NULL);

          SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 3);
          tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[0], &call_id_sv, NULL);
          vpop(); /* pop helper func */
        }
      }
      else
      {
        /* Inline word-by-word copy for small, word-aligned struct returns —
         * mirrors the complex-return inline path.  Skips the memmove call
         * (and the intermediate stack temp that would otherwise be needed
         * for &dst), and exposes the stores to DCE/CSE.
         *
         * Only kicks in when the source is already an lvalue in memory and
         * its bytes form a flat word-aligned image (no VLA, no bitfields).
         * Falls back to the generic vstore() path otherwise.
         *
         * IR shape:
         *   sret_ptr   = LOAD func_vc            ; hidden return pointer
         *   for off in 0..size step 4:
         *     tmp_word = LOAD  src + off
         *     STORE *(sret_ptr + off) <- tmp_word
         */
        int s_size, s_align;
        s_size = type_size(func_type, &s_align);
        int has_struct_vla = struct_has_vla_member(func_type);
        /* Cap at 8 bytes to avoid a store-forwarding width-mismatch issue
         * in the IR optimizer: a 4-byte LOAD that should see an earlier
         * wider STORE can incorrectly forward from a stale narrow STORE at
         * the same address.  Triggers on 16-byte vector literals built from
         * scalar components (e.g. (__m128i){a, b} — zero-init + 8-byte
         * stores), pr92618. */
        if (tcc_state->ir && !has_struct_vla && (vtop->r & VT_LVAL) && s_size > 0 &&
            s_size <= 8 && !(s_size & 3) && !(s_align & 3) && !NOEVAL_WANTED)
        {
          SValue src_mem = *vtop;

          SValue sret_slot;
          memset(&sret_slot, 0, sizeof(sret_slot));
          sret_slot.type.t = VT_PTR;
          sret_slot.r = VT_LOCAL | VT_LVAL;
          sret_slot.vr = -1;
          sret_slot.c.i = func_vc;

          SValue sret_ptr;
          memset(&sret_ptr, 0, sizeof(sret_ptr));
          sret_ptr.type.t = VT_PTR;
          sret_ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
          sret_ptr.r = 0;

          tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &sret_slot, NULL, &sret_ptr);

          CType word_type;
          word_type.t = VT_INT;
          word_type.ref = NULL;

          /* Two-pass: all LOADs first, then all STOREs, so a STORE through
           * sret_ptr doesn't act as an alias barrier blocking forwarding to
           * a later LOAD from the source (e.g. parameter spill slot). */
          int n_words = s_size / 4;
          int tmp_vregs[32 / 4];
          /* c.i on a register-deref lvalue (e.g. `return *p;`) is not a
           * frame offset and is ignored by the codegen — compute src + off
           * explicitly via ADD, like the vstore() inline copy does. */
          int src_is_reg_deref = ((src_mem.r & VT_VALMASK) < VT_CONST);
          for (int i = 0; i < n_words; ++i)
          {
            int off = i * 4;
            SValue src_word = src_mem;
            src_word.type = word_type;

            if (src_is_reg_deref && off != 0)
            {
              SValue off_imm;
              svalue_init(&off_imm);
              off_imm.type.t = VT_INT;
              off_imm.r = VT_CONST;
              off_imm.vr = -1;
              off_imm.c.i = off;

              SValue src_base;
              memset(&src_base, 0, sizeof(src_base));
              src_base.type.t = VT_PTR;
              src_base.vr = src_mem.vr;
              src_base.r = 0;

              SValue src_ptr;
              svalue_init(&src_ptr);
              src_ptr.type.t = VT_PTR;
              src_ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
              src_ptr.r = 0;

              tcc_ir_put(tcc_state->ir, TCCIR_OP_ADD, &src_base, &off_imm, &src_ptr);

              src_word.r = VT_LVAL;
              src_word.vr = src_ptr.vr;
              src_word.sym = NULL;
              src_word.c.i = 0;
            }
            else
            {
              src_word.c.i += off;
            }

            SValue tmp_word;
            svalue_init(&tmp_word);
            tmp_word.type = word_type;
            tmp_word.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
            tmp_word.r = 0;

            tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &src_word, NULL, &tmp_word);
            tmp_vregs[i] = tmp_word.vr;
          }

          for (int i = 0; i < n_words; ++i)
          {
            int off = i * 4;
            SValue tmp_word;
            svalue_init(&tmp_word);
            tmp_word.type = word_type;
            tmp_word.vr = tmp_vregs[i];
            tmp_word.r = 0;

            SValue dst_ptr;
            if (off == 0)
            {
              dst_ptr = sret_ptr;
            }
            else
            {
              SValue off_imm;
              svalue_init(&off_imm);
              off_imm.type.t = VT_INT;
              off_imm.r = VT_CONST;
              off_imm.vr = -1;
              off_imm.c.i = off;

              svalue_init(&dst_ptr);
              dst_ptr.type.t = VT_PTR;
              dst_ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
              dst_ptr.r = 0;

              tcc_ir_put(tcc_state->ir, TCCIR_OP_ADD, &sret_ptr, &off_imm, &dst_ptr);
            }

            SValue store_dst;
            svalue_init(&store_dst);
            store_dst.type = word_type;
            store_dst.r = VT_LVAL;
            store_dst.vr = dst_ptr.vr;

            tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &tmp_word, NULL, &store_dst);
          }
        }
        else
        {
          /* if returning structure, must copy it to implicit
             first pointer arg location */
          type = *func_type;
          mk_pointer(&type);
          vset(&type, VT_LOCAL | VT_LVAL, func_vc);
          indir();
          vswap();
          /* copy structure value to pointer */
          vstore();
        }
      }
    }
    else
    {
      /* returning structure packed into registers */
      int size, addr, align, rc, n;
      size = type_size(func_type, &align);
      /* For tiny (1- or 2-byte) GCC vectors, skip the misalignment fixup
       * and load the value at its actual width.  The standard path widens
       * the LOAD to ret_type (VT_INT, 4 bytes) and adds a struct-copy to
       * an aligned slot; for single-byte vectors that costs an extra
       * store/load pair just to satisfy the 4-byte alignment of ret_align.
       * AAPCS treats upper bytes of the return register as don't-care for
       * sub-word types, so a narrow zero-extending LOAD is correct. */
      int is_tiny_vec = (func_type->t & VT_VECTOR) && (size == 1 || size == 2) &&
                        ret_nregs == 1;
      if (!is_tiny_vec &&
          (align & (ret_align - 1)) && ((vtop->r & VT_VALMASK) < VT_CONST /* pointer to struct */
                                        || (vtop->c.i & (ret_align - 1))))
      {
        loc = (loc - size) & -ret_align;
        addr = loc;
        type = *func_type;
        vset(&type, VT_LOCAL | VT_LVAL, addr);
        vswap();
        vstore();
        vpop();
        vset(&ret_type, VT_LOCAL | VT_LVAL, addr);
      }
      if (is_tiny_vec)
      {
        vtop->type.t = (size == 1) ? (VT_BYTE | VT_UNSIGNED) : (VT_SHORT | VT_UNSIGNED);
        vtop->type.ref = NULL;
      }
      else
        vtop->type = ret_type;
      rc = RC_RET(ret_type.t);
      // printf("struct return: n:%d t:%02x rc:%02x\n", ret_nregs, ret_type.t,
      // rc);
      for (n = ret_nregs; --n > 0;)
      {
        vdup();
        gv(rc);
        vswap();
        incr_offset(regsize);
        /* We assume that when a structure is returned in multiple
           registers, their classes are consecutive values of the
           suite s(n) = 2^n */
        rc <<= 1;
      }
      gv(rc);
      vtop -= ret_nregs - 1;
      /* Emit RETURNVALUE so the IR codegen knows to place the loaded
         value into the return register (r0).  Without this the vreg
         produced by gv() is never connected to the physical return
         register and the caller receives garbage. */
      tcc_ir_gen_return_value(tcc_state->ir, vtop);
    }
  }
  else
  {
    // function returns scalar value - ensure it's loaded into a value (not lvalue)
    // This generates proper LOAD IR if vtop is still an lvalue
    if (vtop->r & VT_LVAL)
    {
      /* Load the value first - this ensures proper size is used */
      SValue dest;
      svalue_init(&dest);
      dest.type = vtop->type;
      dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      dest.r = 0;
      dest.c.i = 0;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &dest);
      vtop->vr = dest.vr;
      vtop->r = 0; /* no longer an lvalue */
    }
    tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
    tcc_ir_gen_return_value(tcc_state->ir, vtop);
  }
  vtop--; /* NOT vpop() because on x86 it would flush the fp stack */
  print_vstack("gfunc_return");
}
#endif

void check_func_return(void)
{
  if ((func_vt.t & VT_BTYPE) == VT_VOID)
    return;
  if (!strcmp(funcname, "main") && (func_vt.t & VT_BTYPE) == VT_INT)
  {
    /* main returns 0 by default */
    vpushi(0);
    gen_assign_cast(&func_vt);
    gfunc_return(&func_vt);
  }
  else if (!tcc_state->ir_late_reopt_phase)
  {
    /* Skip during the end-of-TU re-compile: the warning was already emitted
     * during the first-pass compile, and re-emitting would double-report. */
    tcc_warning("function might return no value: '%s'", funcname);
  }
}
