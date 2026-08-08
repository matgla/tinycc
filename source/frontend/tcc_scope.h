/*
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
 
#ifndef TCC_SCOPE_H
#define TCC_SCOPE_H

struct scope
{
  struct scope *prev;
  struct
  {
    int loc, locorig, num;
  } vla;
  struct
  {
    Sym *s;
    int n;
  } cl;
  int *bsym, *csym;
  Sym *lstk, *llstk;
};

extern struct scope *cur_scope, *loop_scope, *root_scope;

#endif /* TCC_SCOPE_H */
