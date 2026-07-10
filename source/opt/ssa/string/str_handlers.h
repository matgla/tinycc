/*
 *  TCC SSA opt - constant string/memory builtin fold handler interface
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_SSA_STR_HANDLERS_H
#define TCC_OPT_SSA_STR_HANDLERS_H

#include "ssa_opt.h"

struct TCCIRState;

/* Per-call folding context handed to a handler. Populated by the driver for
 * one FUNCCALL* whose callee resolved to the handler's StrBuiltinId. */
typedef struct StrFoldCtx
{
  IRSSAOptCtx *ssa;
  struct TCCIRState *ir;
  int call_idx;   /* index of the FUNCCALL* instruction */
  int builtin_id; /* StrBuiltinId resolved from the callee */
  int is_valued;  /* 1 = FUNCCALLVAL, 0 = FUNCCALLVOID */
} StrFoldCtx;

/* A handler owns one StrBuiltinId. can_fold is a pure predicate (no mutation);
 * fold mutates instruction call_idx and returns the change count. */
typedef struct StrFoldHandler
{
  int builtin_id;
  const char *name;
  int (*can_fold)(const StrFoldCtx *c);
  int (*fold)(StrFoldCtx *c);
} StrFoldHandler;

/* One per builtin TU; appended to the registry in const_string_fold.c. */
extern const StrFoldHandler tcc_strfold_strlen;
extern const StrFoldHandler tcc_strfold_strcmp;
extern const StrFoldHandler tcc_strfold_strncmp;
extern const StrFoldHandler tcc_strfold_memcmp;
extern const StrFoldHandler tcc_strfold_strcpy;
extern const StrFoldHandler tcc_strfold_strspn;
extern const StrFoldHandler tcc_strfold_strcspn;
extern const StrFoldHandler tcc_strfold_strchr;
extern const StrFoldHandler tcc_strfold_index;
extern const StrFoldHandler tcc_strfold_strrchr;
extern const StrFoldHandler tcc_strfold_rindex;
extern const StrFoldHandler tcc_strfold_strstr;
extern const StrFoldHandler tcc_strfold_strpbrk;

#endif /* TCC_OPT_SSA_STR_HANDLERS_H */
