/*
 *  tcchelpers.c - small helpers libtcc needs that the tcc driver used to own
 *
 *  The little-endian byte access used by the object and debug writers, and
 *  tcc_is_64bit_operand for the code generators.  They lived in tcctools.c and
 *  tcc.c, which only the tcc executable compiles, so libtcc did not link on its
 *  own (as a shared library it had undefined symbols).
 *
 *  Not compiled on its own: libtcc.c #includes it, and the unit tests that
 *  compile tcc.c / tcctools.c directly #include it beside them.
 */

#include "tcc.h"

uint16_t read16le(unsigned char *p)
{
  return p[0] | (uint16_t)p[1] << 8;
}

void write16le(unsigned char *p, uint16_t x)
{
  p[0] = x & 255;
  p[1] = x >> 8 & 255;
}

uint32_t read32le(unsigned char *p)
{
  return read16le(p) | (uint32_t)read16le(p + 2) << 16;
}

void write32le(unsigned char *p, uint32_t x)
{
  write16le(p, x);
  write16le(p + 2, x >> 16);
}

void add32le(unsigned char *p, int32_t x)
{
  write32le(p, read32le(p) + x);
}

uint64_t read64le(unsigned char *p)
{
  return read32le(p) | (uint64_t)read32le(p + 4) << 32;
}

void write64le(unsigned char *p, uint64_t x)
{
  write32le(p, x);
  write32le(p + 4, x >> 32);
}

ST_FUNC int tcc_is_64bit_operand(SValue *sv)
{
  int vt = 0;
  if (sv == NULL)
  {
    return 0;
  }

  vt = sv->type.t & VT_BTYPE;
  if ((vt == VT_LLONG) || (vt == VT_DOUBLE) || (vt == VT_LDOUBLE))
  {
    return 1;
  }
  return 0;
}
