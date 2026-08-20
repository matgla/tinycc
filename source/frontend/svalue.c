/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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

#include "tcc.h"

void svalue_init(SValue *sv)
{
  sv->pr0_reg = PREG_REG_NONE;
  sv->pr0_spilled = 0;
  sv->underaligned = 0;
  sv->volatile_access = 0;
  sv->pr1_reg = PREG_REG_NONE;
  sv->pr1_spilled = 0;
  sv->r = 0;
  sv->vr = -1;
  sv->type.t = 0;
  sv->type.ref = NULL;
  sv->c.i = 0;
  sv->sym = NULL;
}

SValue svalue_const_i64(int64_t v)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = (uint64_t)v;
  return sv;
}

SValue svalue_call_id(int call_id)
{
  return svalue_const_i64((int64_t)TCCIR_ENCODE_PARAM(call_id, 0));
}

SValue svalue_call_id_argc(int call_id, int argc)
{
  return svalue_const_i64((int64_t)TCCIR_ENCODE_CALL(call_id, argc));
}
