/*
 *  TCC - yasc casts: what converts, and what traps
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* test_yasc_casts.py: argv[1] names a case; it prints "go <case>", runs,
 * and prints "ok <case> <value>" -- or traps on a cast's check (udf, a
 * HardFault on the QEMU board).  Operands come from volatiles. */
#pragma yasc
#include <stdint.h>
#include <stdio.h>
#include <string.h>

volatile int v_m1 = -1, v_0 = 0, v_1 = 1, v_2 = 2, v_4 = 4, v_9 = 9, v_255 = 255, v_256 = 256, v_70000 = 70000;
volatile int v_imin = INT32_MIN;
volatile unsigned v_u3g = 3000000000u;
volatile long long v_big = 0x100000001ll, v_neg64 = -5;
volatile unsigned long long v_u64max = ~0ull;
volatile double v_d = 1234.75, v_dneg = -2.5, v_dhuge = 3e9, v_dnan = 0.0;
volatile float v_f = 1.5f, v_fbig = 70000.0f;

enum op { OP_A, OP_B, OP_C };
enum sp { S1 = 1, S4 = 4, S9 = 9 };
enum wide { W0 = 0, W100 = 100 };

struct dirent { uint32_t d_ino; uint16_t d_reclen; char d_name[2]; };
struct dir { int pos; _Alignas(struct dirent) char buf[32]; };
static struct dir g_dir;

static __attribute__((noinline)) int to_u8(int x) { return cast<int(uint8_t)>(x); }
static __attribute__((noinline)) int to_i8(int x) { return cast<int(int8_t)>(x); }
static __attribute__((noinline)) unsigned to_u32(int x) { return cast<int(uint32_t)>(x); }
static __attribute__((noinline)) int u_to_i32(unsigned x) { return cast<int(int32_t)>(x); }
static __attribute__((noinline)) int ll_to_i32(long long x) { return cast<int(int32_t)>(x); }
static __attribute__((noinline)) unsigned ull_to_u32(unsigned long long x) { return cast<int(uint32_t)>(x); }
static __attribute__((noinline)) unsigned long long i_to_u64(int x) { return cast<int(uint64_t)>(x); }
static __attribute__((noinline)) int d_to_i32(double d) { return cast<int(int32_t)>(d); }
static __attribute__((noinline)) int d_to_u16(double d) { return cast<int(uint16_t)>(d); }
static __attribute__((noinline)) int f_to_u16(float f) { return cast<int(uint16_t)>(f); }
static __attribute__((noinline)) int to_op(int x) { return cast<int(int)>(cast<enum(enum op)>(x)); }
static __attribute__((noinline)) int to_sp(int x) { return cast<int(int)>(cast<enum(enum sp)>(x)); }
static __attribute__((noinline)) int to_wide(int x) { return cast<int(int)>(cast<enum(enum wide)>(x)); }
static __attribute__((noinline)) int trunc8(int x) { return cast<trunc(uint8_t)>(x); }
static __attribute__((noinline)) int dirent_at(int pos)
{
  const struct dirent * view d = cast<ptr(const struct dirent *), align>(g_dir.buf + pos);
  return d->d_reclen;
}
static __attribute__((noinline)) int aligned_at(int pos)
{
  const char * view p = cast<align(4)>(g_dir.buf + pos);
  return p[0];
}

int main(int argc, const char * view *argv)
{
  const char * view c = argc > 1 ? argv[1] : "";
  long long r = 0;
  printf("go %s\n", c);
  memset(&g_dir, 0, sizeof g_dir);
  g_dir.buf[4] = 7;
  g_dir.buf[8 + 4] = 9;
  if (!strcmp(c, "u8_ok"))
    r = to_u8(v_255);
  else if (!strcmp(c, "u8_over"))
    r = to_u8(v_256);
  else if (!strcmp(c, "u8_neg"))
    r = to_u8(v_m1);
  else if (!strcmp(c, "i8_ok"))
    r = to_i8(-v_255 / 2);
  else if (!strcmp(c, "i8_over"))
    r = to_i8(v_255);
  else if (!strcmp(c, "u32_neg"))
    r = to_u32(v_m1);
  else if (!strcmp(c, "u32_ok"))
    r = to_u32(v_70000);
  else if (!strcmp(c, "u_i32_over"))
    r = u_to_i32(v_u3g);
  else if (!strcmp(c, "ll_i32_ok"))
    r = ll_to_i32(v_neg64);
  else if (!strcmp(c, "ll_i32_over"))
    r = ll_to_i32(v_big);
  else if (!strcmp(c, "ull_u32_over"))
    r = ull_to_u32(v_u64max);
  else if (!strcmp(c, "i_u64_neg"))
    r = cast<trunc(long long)>(i_to_u64(v_imin));
  else if (!strcmp(c, "i_u64_ok"))
    r = cast<trunc(long long)>(i_to_u64(v_70000));
  else if (!strcmp(c, "d_i32_ok"))
    r = d_to_i32(v_d);
  else if (!strcmp(c, "d_i32_neg"))
    r = d_to_i32(v_dneg);
  else if (!strcmp(c, "d_i32_over"))
    r = d_to_i32(v_dhuge);
  else if (!strcmp(c, "d_nan"))
    r = d_to_i32(v_dnan / v_dnan);
  else if (!strcmp(c, "d_u16_neg"))
    r = d_to_u16(v_dneg);
  else if (!strcmp(c, "f_u16_ok"))
    r = f_to_u16(v_f);
  else if (!strcmp(c, "f_u16_over"))
    r = f_to_u16(v_fbig);
  else if (!strcmp(c, "enum_ok"))
    r = to_op(v_2);
  else if (!strcmp(c, "enum_over"))
    r = to_op(v_4);
  else if (!strcmp(c, "enum_neg"))
    r = to_op(v_m1);
  else if (!strcmp(c, "enum_sparse_ok"))
    r = to_sp(v_9);
  else if (!strcmp(c, "enum_sparse_gap"))
    r = to_sp(v_2);
  else if (!strcmp(c, "enum_wide_ok"))
    r = to_wide(v_0);
  else if (!strcmp(c, "enum_wide_gap"))
    r = to_wide(v_9);
  else if (!strcmp(c, "trunc"))
    r = trunc8(v_256 + v_9);
  else if (!strcmp(c, "align_ok"))
    r = dirent_at(v_4 * 2);
  else if (!strcmp(c, "align_bad"))
    r = dirent_at(v_2);
  else if (!strcmp(c, "align_n_ok"))
    r = aligned_at(v_4);
  else if (!strcmp(c, "align_n_bad"))
    r = aligned_at(v_1);
  else if (!strcmp(c, "bits"))
    r = cast<bits(uint32_t)>(v_f);
  else if (!strcmp(c, "bits_back"))
    r = cast<int(int)>(cast<bits(float)>(0x40400000u) * 2.0f);
  else if (!strcmp(c, "float"))
    r = cast<int(int)>(cast<float(float)>(v_u3g) / 1000.0f);
  printf("ok %s %lld\n", c, r);
  return 0;
}
