/*
 *  TCC IR - Function Write Summary: shared record type + lookup
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_FLAT_IPA_FUNC_WRITE_SUMMARY_H
#define TCC_OPT_FLAT_IPA_FUNC_WRITE_SUMMARY_H

#define FWS_MAX_PARAMS 8  /* up to 8 tracked pointer params per function */
#define FWS_MAX_BYTES 256 /* up to 256 bytes per param */

typedef struct FwsParamSummary
{
  int param_idx;                         /* IR param index this entry covers */
  uint8_t must_write[FWS_MAX_BYTES / 8]; /* bit i set = byte i is must-write */
} FwsParamSummary;

typedef struct FuncWriteSummary
{
  int num_params;
  FwsParamSummary params[FWS_MAX_PARAMS];
} FuncWriteSummary;

FuncWriteSummary *fws_lookup(Sym *sym);
int fws_range_fully_set(const FwsParamSummary *ps, int offset, int size);
int fws_btype_bytes(int btype);

#endif
