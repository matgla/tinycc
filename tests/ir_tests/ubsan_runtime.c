/*
 Copyright (c) 2026 Mateusz Stadnik

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/* test_sanitize.py's copy of yasos libc's ubsan.c (branch
 * feat/ubsan-handlers), for the newlib QEMU runs: keep the two the same.
 *
 * The runtime of tcc's -fsanitize=undefined in handler mode
 * (-fno-sanitize-trap, -fsanitize-recover, -fno-sanitize-recover): each
 * failing check calls one of these with clang's data record (the source
 * location and type descriptors) and its operands, and the report goes to
 * stderr as clang's does:
 *
 *   f.c:12: runtime error: signed integer overflow: 2147483647 + 1 cannot be
 *   represented in type 'int'
 *
 * The plain handlers report each place once (the record's column is the
 * mark, as clang's) and return: the program goes on with the wrapped value.
 * The _abort ones (-fno-sanitize-recover) report and abort.
 *
 * A value travels as clang's ValueHandle: an integer no wider than a pointer
 * in the handle itself, a wider one (long long) behind a pointer to it. */

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

struct ubsan_loc
{
  const char *file;
  uint32_t line;
  uint32_t column;
};

/* kind 0: an integer, info = log2(bits) << 1 | signed; 1: a float, info =
   bits; 0xffff: anything else */
struct ubsan_type
{
  uint16_t kind;
  uint16_t info;
  char name[1];
};

struct ubsan_overflow
{
  struct ubsan_loc loc;
  const struct ubsan_type *type;
};

struct ubsan_shift
{
  struct ubsan_loc loc;
  const struct ubsan_type *lhs;
  const struct ubsan_type *rhs;
};

struct ubsan_mismatch
{
  struct ubsan_loc loc;
  const struct ubsan_type *type;
  unsigned char log_align;
  unsigned char check_kind;
};

struct ubsan_vla
{
  struct ubsan_loc loc;
  const struct ubsan_type *type;
};

struct ubsan_oob
{
  struct ubsan_loc loc;
  const struct ubsan_type *array;
  const struct ubsan_type *index;
};

/* the report being written */
struct ubsan_out
{
  char buf[256];
  unsigned n;
};

static void ub_str(struct ubsan_out *o, const char *s)
{
  while (*s && o->n < sizeof o->buf - 1)
    o->buf[o->n++] = *s++;
}

static void ub_udec(struct ubsan_out *o, uint64_t v)
{
  char t[24];
  int i = sizeof t - 1;
  t[i] = 0;
  do
    t[--i] = (char)('0' + v % 10);
  while ((v /= 10) && i > 0);
  ub_str(o, t + i);
}

static void ub_hex(struct ubsan_out *o, uintptr_t v)
{
  char t[2 + 2 * sizeof v + 1];
  int i = sizeof t - 1;
  t[i] = 0;
  do
    t[--i] = "0123456789abcdef"[v & 15];
  while ((v >>= 4) && i > 2);
  t[--i] = 'x';
  t[--i] = '0';
  ub_str(o, t + i);
}

static unsigned ub_bits(const struct ubsan_type *t)
{
  if (t->kind == 0)
    return 1u << (t->info >> 1);
  if (t->kind == 1)
    return t->info;
  return 0;
}

static int ub_signed(const struct ubsan_type *t)
{
  return t->kind == 0 && (t->info & 1);
}

/* the handle's value, zero- or sign-extended to 64 bits */
static uint64_t ub_value(const struct ubsan_type *t, uintptr_t h)
{
  unsigned bits = ub_bits(t);
  uint64_t v;
  if (bits > 8 * sizeof h)
    return bits == 64 ? *(const uint64_t *)h : 0;
  v = h;
  if (bits && bits < 64)
  {
    v &= ((uint64_t)1 << bits) - 1;
    if (ub_signed(t) && (v >> (bits - 1)))
      v |= ~(uint64_t)0 << bits;
  }
  return v;
}

static int ub_negative(const struct ubsan_type *t, uintptr_t h)
{
  return ub_signed(t) && (int64_t)ub_value(t, h) < 0;
}

static void ub_int(struct ubsan_out *o, const struct ubsan_type *t, uintptr_t h)
{
  uint64_t v = ub_value(t, h);
  if (ub_signed(t) && (int64_t)v < 0)
  {
    ub_str(o, "-");
    v = -v;
  }
  ub_udec(o, v);
}

static void ub_type(struct ubsan_out *o, const struct ubsan_type *t)
{
  ub_str(o, "'");
  ub_str(o, t->name);
  ub_str(o, "'");
}

/* 0: this place was reported already (a recoverable handler says it once) */
static int ub_begin(struct ubsan_out *o, struct ubsan_loc *loc, int abort_)
{
  uint32_t col = loc->column;
  if (col == 0xffffffffu && !abort_)
    return 0;
  loc->column = 0xffffffffu;
  o->n = 0;
  ub_str(o, loc->file ? loc->file : "<unknown>");
  ub_str(o, ":");
  ub_udec(o, loc->line);
  if (col && col != 0xffffffffu)
  {
    ub_str(o, ":");
    ub_udec(o, col);
  }
  ub_str(o, ": runtime error: ");
  return 1;
}

static void ub_end(struct ubsan_out *o, int abort_)
{
  o->buf[o->n++] = '\n';
  write(2, o->buf, o->n);
  if (abort_)
    abort();
}

static void ub_arith(struct ubsan_overflow *d, uintptr_t a, uintptr_t b, const char *op, int abort_)
{
  struct ubsan_out o;
  if (!ub_begin(&o, &d->loc, abort_))
    return;
  ub_str(&o, ub_signed(d->type) ? "signed" : "unsigned");
  ub_str(&o, " integer overflow: ");
  ub_int(&o, d->type, a);
  ub_str(&o, op);
  ub_int(&o, d->type, b);
  ub_str(&o, " cannot be represented in type ");
  ub_type(&o, d->type);
  ub_end(&o, abort_);
}

void __ubsan_handle_add_overflow(struct ubsan_overflow *d, uintptr_t a, uintptr_t b)
{
  ub_arith(d, a, b, " + ", 0);
}

void __ubsan_handle_add_overflow_abort(struct ubsan_overflow *d, uintptr_t a, uintptr_t b)
{
  ub_arith(d, a, b, " + ", 1);
}

void __ubsan_handle_sub_overflow(struct ubsan_overflow *d, uintptr_t a, uintptr_t b)
{
  ub_arith(d, a, b, " - ", 0);
}

void __ubsan_handle_sub_overflow_abort(struct ubsan_overflow *d, uintptr_t a, uintptr_t b)
{
  ub_arith(d, a, b, " - ", 1);
}

void __ubsan_handle_mul_overflow(struct ubsan_overflow *d, uintptr_t a, uintptr_t b)
{
  ub_arith(d, a, b, " * ", 0);
}

void __ubsan_handle_mul_overflow_abort(struct ubsan_overflow *d, uintptr_t a, uintptr_t b)
{
  ub_arith(d, a, b, " * ", 1);
}

static void ub_negate(struct ubsan_overflow *d, uintptr_t a, int abort_)
{
  struct ubsan_out o;
  if (!ub_begin(&o, &d->loc, abort_))
    return;
  ub_str(&o, "negation of ");
  ub_int(&o, d->type, a);
  ub_str(&o, " cannot be represented in type ");
  ub_type(&o, d->type);
  if (ub_signed(d->type))
    ub_str(&o, "; cast to an unsigned type to negate this value to itself");
  ub_end(&o, abort_);
}

void __ubsan_handle_negate_overflow(struct ubsan_overflow *d, uintptr_t a)
{
  ub_negate(d, a, 0);
}

void __ubsan_handle_negate_overflow_abort(struct ubsan_overflow *d, uintptr_t a)
{
  ub_negate(d, a, 1);
}

static void ub_divrem(struct ubsan_overflow *d, uintptr_t a, uintptr_t b, int abort_)
{
  struct ubsan_out o;
  if (!ub_begin(&o, &d->loc, abort_))
    return;
  if (d->type->kind == 0 && ub_value(d->type, b) == 0)
    ub_str(&o, "division by zero");
  else
  {
    ub_str(&o, "division of ");
    ub_int(&o, d->type, a);
    ub_str(&o, " by ");
    ub_int(&o, d->type, b);
    ub_str(&o, " cannot be represented in type ");
    ub_type(&o, d->type);
  }
  ub_end(&o, abort_);
}

void __ubsan_handle_divrem_overflow(struct ubsan_overflow *d, uintptr_t a, uintptr_t b)
{
  ub_divrem(d, a, b, 0);
}

void __ubsan_handle_divrem_overflow_abort(struct ubsan_overflow *d, uintptr_t a, uintptr_t b)
{
  ub_divrem(d, a, b, 1);
}

static void ub_shift(struct ubsan_shift *d, uintptr_t a, uintptr_t b, int abort_)
{
  struct ubsan_out o;
  if (!ub_begin(&o, &d->loc, abort_))
    return;
  if (ub_negative(d->rhs, b))
  {
    ub_str(&o, "shift exponent ");
    ub_int(&o, d->rhs, b);
    ub_str(&o, " is negative");
  }
  else if (ub_value(d->rhs, b) >= ub_bits(d->lhs))
  {
    ub_str(&o, "shift exponent ");
    ub_int(&o, d->rhs, b);
    ub_str(&o, " is too large for ");
    ub_udec(&o, ub_bits(d->lhs));
    ub_str(&o, "-bit type ");
    ub_type(&o, d->lhs);
  }
  else if (ub_negative(d->lhs, a))
  {
    ub_str(&o, "left shift of negative value ");
    ub_int(&o, d->lhs, a);
  }
  else
  {
    ub_str(&o, "left shift of ");
    ub_int(&o, d->lhs, a);
    ub_str(&o, " by ");
    ub_int(&o, d->rhs, b);
    ub_str(&o, " places cannot be represented in type ");
    ub_type(&o, d->lhs);
  }
  ub_end(&o, abort_);
}

void __ubsan_handle_shift_out_of_bounds(struct ubsan_shift *d, uintptr_t a, uintptr_t b)
{
  ub_shift(d, a, b, 0);
}

void __ubsan_handle_shift_out_of_bounds_abort(struct ubsan_shift *d, uintptr_t a, uintptr_t b)
{
  ub_shift(d, a, b, 1);
}

static void ub_mismatch(struct ubsan_mismatch *d, uintptr_t p, int abort_)
{
  static const char *const what[] = {"load of", "store to", "reference binding to", "member access within",
                                     "member call on", "constructor call on", "downcast of", "downcast of",
                                     "upcast of", "cast to virtual base of", "_Nonnull binding to",
                                     "dynamic operation on"};
  uintptr_t align = (uintptr_t)1 << d->log_align;
  struct ubsan_out o;
  if (!ub_begin(&o, &d->loc, abort_))
    return;
  ub_str(&o, d->check_kind < sizeof what / sizeof what[0] ? what[d->check_kind] : "access to");
  if (!p)
  {
    ub_str(&o, " null pointer of type ");
    ub_type(&o, d->type);
  }
  else if (p & (align - 1))
  {
    ub_str(&o, " misaligned address ");
    ub_hex(&o, p);
    ub_str(&o, " for type ");
    ub_type(&o, d->type);
    ub_str(&o, ", which requires ");
    ub_udec(&o, align);
    ub_str(&o, " byte alignment");
  }
  else
  {
    ub_str(&o, " address ");
    ub_hex(&o, p);
    ub_str(&o, " with insufficient space for an object of type ");
    ub_type(&o, d->type);
  }
  ub_end(&o, abort_);
}

void __ubsan_handle_type_mismatch_v1(struct ubsan_mismatch *d, uintptr_t p)
{
  ub_mismatch(d, p, 0);
}

void __ubsan_handle_type_mismatch_v1_abort(struct ubsan_mismatch *d, uintptr_t p)
{
  ub_mismatch(d, p, 1);
}

static void ub_vla(struct ubsan_vla *d, uintptr_t v, int abort_)
{
  struct ubsan_out o;
  if (!ub_begin(&o, &d->loc, abort_))
    return;
  ub_str(&o, "variable length array bound evaluates to non-positive value ");
  ub_int(&o, d->type, v);
  ub_end(&o, abort_);
}

void __ubsan_handle_vla_bound_not_positive(struct ubsan_vla *d, uintptr_t v)
{
  ub_vla(d, v, 0);
}

void __ubsan_handle_vla_bound_not_positive_abort(struct ubsan_vla *d, uintptr_t v)
{
  ub_vla(d, v, 1);
}

static void ub_oob(struct ubsan_oob *d, uintptr_t i, int abort_)
{
  struct ubsan_out o;
  if (!ub_begin(&o, &d->loc, abort_))
    return;
  ub_str(&o, "index ");
  ub_int(&o, d->index, i);
  ub_str(&o, " out of bounds for type ");
  ub_type(&o, d->array);
  ub_end(&o, abort_);
}

void __ubsan_handle_out_of_bounds(struct ubsan_oob *d, uintptr_t i)
{
  ub_oob(d, i, 0);
}

void __ubsan_handle_out_of_bounds_abort(struct ubsan_oob *d, uintptr_t i)
{
  ub_oob(d, i, 1);
}
