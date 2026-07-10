/*
 *  TCC IR — Optimization DSL: Generator Table Entry
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_DSL_ENTRY_H
#define TCC_OPT_DSL_ENTRY_H

/* Generator-table entry macros. See docs/optimizations/opt_dsl_framework.md. */

/* SSA engine entry: OPT_GEN_ENTRY(name, OP) -> { OP, opt_dsl_dispatch_name, "name" } */
#define OPT_GEN_ENTRY(name, op) \
  { op, opt_dsl_dispatch_##name, #name }

/* Flat (pre-SSA) engine entry: OPT_GEN_ENTRY_FLAT(name, OP) -> { OP, fn, "name", 0 } */
#define OPT_GEN_ENTRY_FLAT(name, op) \
  { op, opt_dsl_dispatch_##name##_flat, #name, 0 }

/* Count entries in a DSL table. */
#define OPT_DSL_TABLE_COUNT(tbl) \
  (sizeof(tbl) / sizeof((tbl)[0]))

#endif /* TCC_OPT_DSL_ENTRY_H */
