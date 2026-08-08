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

/* load.c -- Register loading (gv), address-of lowering, temp locals, bitfields and bounds checks.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Legacy register spilling helpers removed: IR owns spilling. */

/* IR-only: frontend never allocates physical registers. */

/* find a free temporary local variable (return the offset on stack) match
   size and align. If none, add new temporary stack variable.
   The temp local index is encoded in vr_out using VR_TEMP_LOCAL(). */
int get_temp_local_var(int size, int align, int *vr_out)
{
  int i;
  struct temp_local_variable *temp_var;
  SValue *p;
  int r;
  unsigned used = 0;

  /* mark locations that are still in use */
  for (p = vstack; p <= vtop; p++)
  {
    r = p->r & VT_VALMASK;
    if (r == VT_LOCAL || r == VT_LLOCAL)
    {
      if (VR_IS_TEMP_LOCAL(p->vr))
        used |= 1 << VR_TEMP_LOCAL_IDX(p->vr);
    }
  }
  for (i = 0; i < nb_temp_local_vars; i++)
  {
    temp_var = &arr_temp_local_vars[i];
    if (!(used & 1 << i) && temp_var->size >= size && temp_var->align >= align)
    {
    ret_tmp:
      *vr_out = VR_TEMP_LOCAL(i);
      /* The slot no longer holds whatever a vector recipe recorded for it. */
      vec_recipe_kill_slot(VR_TEMP_LOCAL(i));
      return temp_var->location;
    }
  }
  loc = (loc - size) & -align;
  if (nb_temp_local_vars < MAX_TEMP_LOCAL_VARIABLE_NUMBER)
  {
    temp_var = &arr_temp_local_vars[nb_temp_local_vars];
    temp_var->location = loc;
    temp_var->size = size;
    temp_var->align = align;
    nb_temp_local_vars++;
    goto ret_tmp;
  }
  *vr_out = -1; /* No temp local slot available */
  return loc;
}

/* move register 's' (of type 't') to 'r', and flush previous value of r to
   memory if needed */
void move_reg(int r, int s, int t)
{
  (void)r;
  (void)s;
  (void)t;
  /* IR-only: physical register shuffling is handled after IR lowering. */
  return;
}

/* get address of vtop (vtop MUST BE an lvalue) */
ST_FUNC void gaddrof(void)
{
  /* If the address of a tracked const-init local is being taken, the
   * subsequent operations could write through the derived pointer and
   * desync the captured buffer. Invalidate eagerly so callers that
   * relied on the captured data must have read it before this point. */
  if ((vtop->r & (VT_VALMASK | VT_LVAL)) == (VT_LOCAL | VT_LVAL))
  {
    int off = (int)vtop->c.i;
    Sym *s;
    for (s = local_stack; s; s = s->prev)
    {
      if (!s->const_init_data || !s->const_init_valid)
        continue;
      if (s->const_init_in_progress)
        continue;
      if ((int)s->c == off)
      {
        s->const_init_valid = 0;
        break;
      }
    }
  }
  vtop->r &= ~VT_LVAL;
  /* tricky: if saved lvalue, then we can go back to lvalue */
  if ((vtop->r & VT_VALMASK) == VT_LLOCAL)
  {
    if (nocode_wanted)
      return;

    /* VT_LLOCAL means the pointer is stored at the local/param location.
     * We need to load that pointer value into a temporary. */
    SValue ptr_location = *vtop; // Save the location where the pointer is stored

    // Convert VT_LLOCAL to VT_LOCAL so backend knows it's a stack/param location
    ptr_location.r = (ptr_location.r & ~VT_VALMASK) | VT_LOCAL | VT_LVAL;
    // ptr_location should have pointer type, not struct type
    // This tells the backend that loading from this location gives us a pointer value
    ptr_location.type = vtop->type; // Keep the pointer type from the original VT_LLOCAL parameter

    SValue loaded_ptr;
    memset(&loaded_ptr, 0, sizeof(loaded_ptr));
    loaded_ptr.type = *pointed_type(&vtop->type);                 // Type of what the pointer points to
    loaded_ptr.type.t = (loaded_ptr.type.t & ~VT_BTYPE) | VT_PTR; // Make it a pointer type
    loaded_ptr.type.t &= ~(VT_ARRAY | VT_VLA);
    loaded_ptr.type.ref = vtop->type.ref;
    loaded_ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);

    // Generate LOAD operation: loaded_ptr <-- *ptr_location
    tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &ptr_location, NULL, &loaded_ptr);

    // Replace vtop with the loaded pointer
    *vtop = loaded_ptr;
    // The loaded pointer is the address value itself, NOT an lvalue.
    // We loaded the pointer from the stack slot; this pointer IS the base address.
    // Do NOT set VT_LVAL here - that would cause another dereference when we
    // want to do pointer arithmetic (e.g., adding field offset).
    vtop->r = 0;
  }
  else if ((vtop->r & VT_VALMASK) == VT_LOCAL && tcc_state->ir)
  {
    /* In nocode_wanted mode (e.g. __builtin_object_size), preserve the
     * VT_LOCAL address + c.i offset for compile-time analysis instead
     * of emitting LEA that destroys this information. */
    if (nocode_wanted)
      return;

    /* VT_LOCAL without VT_LVAL means "address of local variable".
     * In IR mode, emit explicit LEA to compute FP+offset into a vreg.
     * This avoids ambiguity where VT_LOCAL alone could be misinterpreted
     * as either "address value" or "spilled value to load".
     *
     * IMPORTANT: Do NOT set VT_LVAL here! LEA needs the raw VT_LOCAL
     * so that tcc_ir_materialize_addr() computes the stack address.
     * VT_LVAL would prevent address materialization.
     */
    SValue src = *vtop;
    /* Ensure VT_LOCAL is preserved and VT_LVAL is NOT set */
    src.r = (src.r & ~VT_LVAL) | VT_LOCAL;

    SValue dest;
    memset(&dest, 0, sizeof(dest));
    dest.type.t = VT_PTR;
    dest.type.ref = vtop->type.ref;
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);

    tcc_ir_put(tcc_state->ir, TCCIR_OP_LEA, &src, NULL, &dest);

    vtop->vr = dest.vr;
    vtop->r = 0; /* Now it's a computed value in a vreg */
    vtop->c.i = 0;
  }
  else if ((vtop->r & VT_PARAM) && tcc_state->ir && (vtop->r & VT_VALMASK) < VT_CONST)
  {
    if (nocode_wanted)
      return;

    /* Register-passed parameter without VT_LOCAL: in IR mode, register
     * parameters are represented as VT_PARAM | VT_LVAL (val_kind = register
     * number) without VT_LOCAL. When address-of is applied, the parameter
     * must reside on the stack (addrtaken was already set on the vreg).
     * Emit a LEA referencing the vreg as VT_LOCAL so the register allocator
     * assigns a stack slot and the LEA computes its address.
     */
    SValue src = *vtop;
    /* Convert to VT_LOCAL (with VT_PARAM preserved) so svalue_to_iroperand
     * classifies it as IROP_TAG_STACKOFF with is_param=1. */
    src.r = VT_LOCAL | VT_PARAM;

    SValue dest;
    memset(&dest, 0, sizeof(dest));
    dest.type.t = VT_PTR;
    dest.type.ref = vtop->type.ref;
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);

    tcc_ir_put(tcc_state->ir, TCCIR_OP_LEA, &src, NULL, &dest);

    vtop->vr = dest.vr;
    vtop->r = 0; /* Now it's a computed value in a vreg */
    vtop->c.i = 0;
  }
}

#ifdef CONFIG_TCC_BCHECK
/* generate a bounded pointer addition */
void gen_bounded_ptr_add(void)
{
  int save = (vtop[-1].r & VT_VALMASK) == VT_LOCAL;
  if (save)
  {
    vpushv(&vtop[-1]);
    vrott(3);
  }
  vpush_helper_func(TOK___bound_ptr_add);
  vrott(3);
  // gfunc_call(2);
  tcc_error("1 implement me");
  vtop -= save;
  vpushi(0);
  /* returned pointer is in REG_IRET */
  vtop->r = REG_IRET | VT_BOUNDED;
  if (nocode_wanted)
    return;
  /* relocation offset of the bounding function call point */
  vtop->c.i = (cur_text_section->reloc->data_offset - sizeof(ElfW_Rel));
}

/* patch pointer addition in vtop so that pointer dereferencing is
   also tested */
static void gen_bounded_ptr_deref(void)
{
  addr_t func;
  int size, align;
  ElfW_Rel *rel;
  Sym *sym;

  if (nocode_wanted)
    return;

  size = type_size(&vtop->type, &align);
  switch (size)
  {
  case 1:
    func = TOK___bound_ptr_indir1;
    break;
  case 2:
    func = TOK___bound_ptr_indir2;
    break;
  case 4:
    func = TOK___bound_ptr_indir4;
    break;
  case 8:
    func = TOK___bound_ptr_indir8;
    break;
  case 12:
    func = TOK___bound_ptr_indir12;
    break;
  case 16:
    func = TOK___bound_ptr_indir16;
    break;
  default:
    /* may happen with struct member access */
    return;
  }
  sym = external_helper_sym(func);
  if (!sym->c)
    put_extern_sym(sym, NULL, 0, 0);
  /* patch relocation */
  /* XXX: find a better solution ? */
  rel = (ElfW_Rel *)(cur_text_section->reloc->data + vtop->c.i);
  rel->r_info = ELFW(R_INFO)(sym->c, ELFW(R_TYPE)(rel->r_info));
}

/* generate lvalue bound code */
void gbound(void)
{
  CType type1;

  vtop->r &= ~VT_MUSTBOUND;
  /* if lvalue, then use checking code before dereferencing */
  if (vtop->r & VT_LVAL)
  {
    /* if not VT_BOUNDED value, then make one */
    if (!(vtop->r & VT_BOUNDED))
    {
      /* must save type because we must set it to int to get pointer */
      type1 = vtop->type;
      vtop->type.t = VT_PTR;
      gaddrof();
      vpushi(0);
      gen_bounded_ptr_add();
      vtop->r |= VT_LVAL;
      vtop->type = type1;
    }
    /* then check for dereferencing */
    gen_bounded_ptr_deref();
  }
}

/* Add bounds for local symbols from S to E (via ->prev) */
static void add_local_bounds(Sym *s, Sym *e)
{
  for (; s != e; s = s->prev)
  {
    if (!s->v || (s->r & VT_VALMASK) != VT_LOCAL)
      continue;
    /* Add arrays/structs/unions because we always take address */
    if ((s->type.t & VT_ARRAY) || (s->type.t & VT_BTYPE) == VT_STRUCT || s->a.addrtaken)
    {
      /* add local bound info */
      int align, size = type_size(&s->type, &align);
      addr_t *bounds_ptr = section_ptr_add(lbounds_section, 2 * sizeof(addr_t));
      bounds_ptr[0] = s->c;
      bounds_ptr[1] = size;
    }
  }
}
#endif

/* Wrapper around sym_pop, that potentially also registers local bounds.  */
void pop_local_syms(Sym *b, int keep)
{
#ifdef CONFIG_TCC_BCHECK
  if (tcc_state->do_bounds_check && !keep && (local_scope || !func_var))
    add_local_bounds(local_stack, b);
#endif
  if (debug_modes)
    tcc_add_debug_info(tcc_state, !local_scope, local_stack, b);
  sym_pop(&local_stack, b, keep);
}

/* increment an lvalue pointer */
void incr_offset(int offset)
{
  int t = vtop->type.t;
  gaddrof();                   /* remove VT_LVAL */
  vtop->type.t = VT_PTRDIFF_T; /* set scalar type */
  vpushs(offset);
  gen_op('+');
  vtop->r |= VT_LVAL;
  vtop->type.t = t;
}

void incr_bf_adr(int o)
{
  vtop->type.t = VT_BYTE | VT_UNSIGNED;
  incr_offset(o);
}

/* single-byte load mode for packed or otherwise unaligned bitfields */
static void load_packed_bf(CType *type, int bit_pos, int bit_size)
{
  int n, o, bits;
  vpush64(type->t & VT_BTYPE, 0); // B X
  bits = 0, o = bit_pos >> 3, bit_pos &= 7;
  do
  {
    vswap(); // X B
    incr_bf_adr(o);
    vdup(); // X B B
    n = 8 - bit_pos;
    if (n > bit_size)
      n = bit_size;
    if (bit_pos)
      vpushi(bit_pos), gen_op(TOK_SHR), bit_pos = 0; // X B Y
    if (n < 8)
      vpushi((1 << n) - 1), gen_op('&');
    gen_cast(type);
    if (bits)
      vpushi(bits), gen_op(TOK_SHL);
    vrotb(3);    // B Y X
    gen_op('|'); // B X
    bits += n, bit_size -= n, o = 1;
  } while (bit_size);
  vswap(), vpop();
  if (!(type->t & VT_UNSIGNED))
  {
    n = ((type->t & VT_BTYPE) == VT_LLONG ? 64 : 32) - bits;
    vpushi(n), gen_op(TOK_SHL);
    vpushi(n), gen_op(TOK_SAR);
  }
}

/* single-byte store mode for packed or otherwise unaligned bitfields */
void store_packed_bf(int bit_pos, int bit_size)
{
  int bits, n, o, m, c;
  c = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  vswap(); // X B
  bits = 0, o = bit_pos >> 3, bit_pos &= 7;
  do
  {
    incr_bf_adr(o);        // X B
    vswap();               // B X
    c ? vdup() : gv_dup(); // B V X
    vrott(3);              // X B V
    if (bits)
      vpushi(bits), gen_op(TOK_SHR);
    if (bit_pos)
      vpushi(bit_pos), gen_op(TOK_SHL);
    n = 8 - bit_pos;
    if (n > bit_size)
      n = bit_size;
    if (n < 8)
    {
      m = ((1 << n) - 1) << bit_pos;
      vpushi(m), gen_op('&'); // X B V1
      vpushv(vtop - 1);       // X B V1 B
      vpushi(m & 0x80 ? ~m & 0x7f : ~m);
      gen_op('&'); // X B V1 B1
      gen_op('|'); // X B V2
    }
    vdup(), vtop[-1] = vtop[-2]; // X B B V2
    vstore(), vpop();            // X B
    bits += n, bit_size -= n, bit_pos = 0, o = 1;
  } while (bit_size);
  vpop(), vpop();
}

int adjust_bf(SValue *sv, int bit_pos, int bit_size)
{
  int t;
  if (0 == sv->type.ref)
    return 0;
  t = sv->type.ref->auxtype;
  if (t != -1 && t != VT_STRUCT)
  {
    sv->type.t = (sv->type.t & ~(VT_BTYPE | VT_LONG)) | t;
    sv->r |= VT_LVAL;
  }
  return t;
}

/* store vtop a register belonging to class 'rc'. lvalues are
   converted to values. Cannot be used if cannot be converted to
   register value (such as structures). */
ST_FUNC int gv(int rc)
{
  int r, r_ok, r2_ok, rc2;
  int bit_pos, bit_size, size, align;
  int vreg = -1;

  /* For IR mode: if we already have a valid vreg computed, no need to do anything.
     Valid vregs have type 1, 2, or 3 in the upper 4 bits. Type 0 is invalid.
     Exception: bitfield values still need extraction (shift/mask) even when
     they already have a vreg — skip the early return for them.
     Exception: VT_CMP/VT_JMP/VT_JMPI values must be materialized into a 0/1
     vreg even when the stale left-operand vreg is still set after a CMP. */
  {
    int vv = vtop->r & VT_VALMASK;
    if (tcc_state->ir && TCCIR_DECODE_VREG_TYPE(vtop->vr) > 0 && !(vtop->r & VT_LVAL) &&
        !(vtop->type.t & VT_BITFIELD) && vv != VT_CMP && vv != VT_JMP && vv != VT_JMPI)
    {
      return vv;
    }
  }

  /* NOTE: get_reg can modify vstack[] */
  if (vtop->type.t & VT_BITFIELD)
  {
    CType type;

    bit_pos = BIT_POS(vtop->type.t);
    bit_size = BIT_SIZE(vtop->type.t);
    /* remove bit field info to avoid loops */
    vtop->type.t &= ~VT_STRUCT_MASK;

    type.ref = NULL;
    type.t = vtop->type.t & VT_UNSIGNED;
    if ((vtop->type.t & VT_BTYPE) == VT_BOOL)
      type.t |= VT_UNSIGNED;

    r = adjust_bf(vtop, bit_pos, bit_size);

    if ((vtop->type.t & VT_BTYPE) == VT_LLONG)
      type.t |= VT_LLONG;
    else
      type.t |= VT_INT;

    if (r == VT_STRUCT)
    {
      load_packed_bf(&type, bit_pos, bit_size);
    }
    else
    {
      int bits = (type.t & VT_BTYPE) == VT_LLONG ? 64 : 32;
      /* cast to int to propagate signedness in following ops */
      gen_cast(&type);
      /* generate shifts */
      vpushi(bits - (bit_pos + bit_size));
      gen_op(TOK_SHL);
      vpushi(bits - bit_size);
      /* NOTE: transformed to SHR if unsigned */
      gen_op(TOK_SAR);
      vreg = gv(rc);
    }
    r = gv(rc);
  }
  else
  {
    if (is_float(vtop->type.t) && (vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST)
    {
      /* CPUs usually cannot use float constants, so we store them
         generically in data segment */
      init_params p = {rodata_section};
      unsigned long offset;
      size = type_size(&vtop->type, &align);
      if (NODATA_WANTED)
        size = 0, align = 1;
      offset = section_add(p.sec, size, align);
      vpush_ref(&vtop->type, p.sec, offset, size);
      vswap();
      init_putv(&p, &vtop->type, offset, -1);
      vtop->r |= VT_LVAL;
    }
#ifdef CONFIG_TCC_BCHECK
    if (vtop->r & VT_MUSTBOUND)
      gbound();
#endif

    /* Arrays (including VLAs) are not values you can load from memory.
     * In most expressions they decay to a pointer to their first element.
     * If we treat them as an lvalue and "load" them, we end up
     * dereferencing the computed pointer and accidentally using a[0]
     * (or addr[0]) instead of the address itself.
     *
     * This is particularly visible in tests/tests2/79_vla_continue.c where
     * `addr[count] = a;` must store the pointer value of `a`.
     */
    if ((vtop->r & VT_LVAL) && (vtop->type.t & (VT_ARRAY | VT_VLA)))
    {
      gaddrof();
      vtop->type.t &= ~(VT_ARRAY | VT_VLA);
    }

    rc2 = RC_INT; // RC2_TYPE(bt, rc);

    /* need to reload if:
       - constant
       - lvalue (need to dereference pointer)
       - already a register, but not in the right class */
    r = vtop->r & VT_VALMASK;
    r_ok = !(vtop->r & VT_LVAL) && (r < VT_CONST) && (reg_classes[r] & rc);
    r2_ok = !rc2;

    if (tcc_state->ir == NULL)
    {
      if (!nocode_wanted)
        tcc_error("IR-only: gv() requires IR");
      return 0;
    }

    if (tcc_state->ir && rc2)
    {
      /* IR mode: treat 64-bit values as a single vreg, even on targets where
       * the legacy backend would split into two registers.
       *
       * Always materialize into a vreg if we don't already have one.
       */
      if (vtop->vr == -1 || (vtop->r & VT_LVAL) || (vtop->r & VT_VALMASK) >= VT_CONST)
      {
        int vreg = tcc_ir_get_vreg_temp(tcc_state->ir);
        if (is_float(vtop->type.t))
        {
          int is_double = (vtop->type.t & VT_BTYPE) == VT_DOUBLE || (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;
          tcc_ir_set_float_type(tcc_state->ir, vreg, 1, is_double);
        }
        else if ((vtop->type.t & VT_BTYPE) == VT_LLONG)
        {
          tcc_ir_set_llong_type(tcc_state->ir, vreg);
        }

        /* If vtop is VT_CMP/VT_JMP/VT_JMPI (e.g. a comparison result restored
         * from a saved SValue in a ternary expression), materialize it
         * into a 0/1 vreg via cmp_jmp_set instead of trying to LOAD from
         * a jump chain (which svalue_to_iroperand cannot encode). */
        {
          int vv = vtop->r & VT_VALMASK;
          if (vv == VT_CMP || vv == VT_JMP || vv == VT_JMPI)
          {
            tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
            return 0;
          }
        }

        vset_VT_JMP();
        SValue dest;
        svalue_init(&dest);
        dest.type = vtop->type;
        dest.vr = vreg;
        tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &dest);

        vtop->vr = vreg;
        vtop->r = 0;
        vtop->c.i = 0;
        vtop->sym = NULL;
      }
      return 0;
    }

    if (!r_ok || !r2_ok)
    {
      /* IR-only: materialize into a vreg; no physical reg allocation. */
      if (rc2)
        tcc_error("IR-only: unexpected legacy 2-reg gv path");

      vreg = tcc_ir_get_vreg_temp(tcc_state->ir);
      if (is_float(vtop->type.t))
      {
        int is_double = (vtop->type.t & VT_BTYPE) == VT_DOUBLE || (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;
        tcc_ir_set_float_type(tcc_state->ir, vreg, 1, is_double);
      }
      else if ((vtop->type.t & VT_BTYPE) == VT_LLONG)
      {
        tcc_ir_set_llong_type(tcc_state->ir, vreg);
      }

      /* Same guard as above: materialize VT_CMP/VT_JMP via cmp_jmp_set. */
      {
        int vv = vtop->r & VT_VALMASK;
        if (vv == VT_CMP || vv == VT_JMP || vv == VT_JMPI)
        {
          tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
          return 0;
        }
      }

      vset_VT_JMP();
      SValue dest;
      svalue_init(&dest);
      dest.type.t = vtop->type.t;
      dest.vr = vreg;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &dest);

      vtop->vr = vreg;
      vtop->r = 0;
      vtop->c.i = 0;
      vtop->sym = NULL;
    }
    /* vtop->vr is set in the IR LOAD/ASSIGN paths when needed */
  }
  return 0;
}

/* generate vtop[-1] and vtop[0] in resp. classes rc1 and rc2 */
ST_FUNC void gv2(int rc1, int rc2)
{
  /* generate more generic register first. But VT_JMP or VT_CMP
     values must be generated first in all cases to avoid possible
     reload errors */
  if (vtop->r != VT_CMP && rc1 <= rc2)
  {
    vswap();
    gv(rc1);
    vswap();
    gv(rc2);
    /* test if reload is needed for first register */
    if ((vtop[-1].r & VT_VALMASK) >= VT_CONST)
    {
      vswap();
      gv(rc1);
      vswap();
    }
  }
  else
  {
    gv(rc2);
    vswap();
    gv(rc1);
    vswap();
    /* test if reload is needed for first register */
    if ((vtop[0].r & VT_VALMASK) >= VT_CONST)
    {
      gv(rc2);
    }
  }
}
/* convert stack entry to register and duplicate its value in another
   register */
void gv_dup(void)
{
  int t;
  SValue sv;

  t = vtop->type.t;

  /* GCC vector types: vectors are multi-word values that don't fit in a
   * single register.  Duplicate by copying the entire vector to a temp
   * stack slot via memcpy (struct copy), then push the temp as an lvalue.
   * Without this, the scalar ASSIGN below would only copy the first 4
   * bytes and gen_op_vector would misinterpret the loaded data as a
   * pointer when accessing individual elements. */
  if (t & VT_VECTOR)
  {
    int size, align, res_vr, res_loc;
    CType vec_type = vtop->type;
    size = type_size(&vec_type, &align);
    res_loc = get_temp_local_var(size, align > 8 ? 8 : align, &res_vr);

    /* Build destination: temp stack slot lvalue */
    SValue dst;
    memset(&dst, 0, sizeof(dst));
    dst.type = vec_type;
    dst.r = VT_LOCAL | VT_LVAL;
    dst.vr = res_vr;
    dst.c.i = res_loc;

    /* Copy vector from source (vtop) to temp slot via struct store (memcpy) */
    vpushv(&dst); /* push destination lvalue */
    vswap();      /* stack: dst, src */
    vstore();     /* struct copy: src → dst */
    vpop();       /* pop assigned value */

    /* Replace vtop with temp slot lvalue and duplicate */
    vpushv(&dst);
    vdup();
    return;
  }

#if PTR_SIZE == 4
  if ((t & VT_BTYPE) == VT_LLONG)
  {
    if (t & VT_BITFIELD)
    {
      gv(RC_INT);
      t = vtop->type.t;
    }
    lexpand();
    gv_dup();
    vswap();
    vrotb(3);
    gv_dup();
    vrotb(4);
    /* stack: H L L1 H1 */
    lbuild(t);
    vrotb(3);
    vrotb(3);
    vswap();
    lbuild(t);
    vswap();
    return;
  }
#endif
  if (t & VT_BITFIELD)
  {
    gv(RC_INT);
    t = vtop->type.t;
  }
  sv.type.t = VT_INT;
  sv.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
  sv.r = 0;
  sv.c.i = 0;
  tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &sv);
  vtop->vr = sv.vr;
  vtop->r = 0;
  vtop->c.i = 0; /* Clear c.i to avoid corrupting later operations */
  vdup();
}
