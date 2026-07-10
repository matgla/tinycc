/*
 *  TCC IR - Return Value Generation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

void tcc_ir_gen_return_value(TCCIRState *ir, SValue *val)
{
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, val, NULL, NULL);
}
